// Cairn CLI - headless image editor: layered documents, drawing tools, layer
// operations, colour conversion and normal-map generation. Same operations the
// editor's image editor exposes, driven by shared code (editor/main/imaging.js).
'use strict';

const { def } = require('../lib/registry');
const imaging = require('../lib/imaging');
const { usage, invalidArg, notFound } = require('../lib/errors');

function setDoc(ctx, doc, filePath) {
    ctx.session._image = doc;
    ctx.session._imagePath = filePath || null;
    return { width: doc.width, height: doc.height, layers: doc.layers.length, path: ctx.session._imagePath };
}

function getDoc(ctx) {
    const doc = ctx.session._image;
    if (!doc) {
        throw usage('no image loaded', { hint: 'run `cairn image.new` or `cairn image.open --file <png>`' });
    }
    return doc;
}

function getLayer(ctx, indexArg) {
    const doc = getDoc(ctx);
    const index = indexArg === undefined || indexArg === null ? -1 : Number(indexArg);
    if (index < 0 || index >= doc.layers.length) {
        throw notFound(`layer ${indexArg} does not exist`, { layers: doc.layers.length });
    }
    return doc.layers[index];
}

function activeLayer(ctx, args) {
    const doc = getDoc(ctx);
    const index = args.layer === undefined ? doc.layers.length - 1 : Number(args.layer);
    return getLayer(ctx, index);
}

function describe(doc) {
    return {
        path: null,
        width: doc.width,
        height: doc.height,
        layerCount: doc.layers.length,
        layers: doc.layers.map((l, i) => ({ index: i, name: l.name, visible: l.visible, opacity: l.opacity })),
    };
}

def('image.new', {
    summary: 'Create a layered image document (one transparent or coloured base layer).',
    args: {
        width: { type: 'int', default: 256, desc: 'Image width in pixels (1-4096).' },
        height: { type: 'int', default: 256, desc: 'Image height in pixels (1-4096).' },
        background: { type: 'string', default: 'white', desc: 'white, black, transparent or a color (hex).' },
        layers: { type: 'int', default: 1, desc: 'Number of initial layers (0 for an empty document).' },
    },
    returns: '{width, height, layerCount, layers}',
    examples: [
        'cairn image.new --width 128 --height 128 --background white',
        'cairn image.new --width 64 --height 64 --background transparent',
    ],
    run(ctx, args) {
        if (args.width < 1 || args.height < 1 || args.width > 4096 || args.height > 4096) {
            throw invalidArg('image size must be between 1 and 4096 pixels');
        }
        let background = [1, 1, 1, 1];
        if (args.background === 'transparent') background = null;
        else if (args.background === 'black') background = [0, 0, 0, 1];
        else if (args.background !== 'white') {
            const parsed = imaging.parseColor(args.background);
            if (!parsed) throw invalidArg(`unknown background '${args.background}'`);
            background = parsed;
        }
        const doc = { width: args.width, height: args.height, layers: [] };
        for (let i = 0; i < Math.max(0, args.layers); ++i) {
            doc.layers.push(Object.assign(imaging.newLayer(args.width, args.height, background),
                { name: args.layers === 1 ? 'Background' : `Layer ${i + 1}` }));
        }
        return setDoc(ctx, doc, null);
    },
});

def('image.open', {
    summary: 'Load a PNG or BMP file as a single-layer document.',
    args: { file: { type: 'string', required: true, desc: 'Image file to load.' } },
    returns: '{width, height, layerCount, path}',
    examples: ['cairn image.open --file Assets/Textures/checker.png'],
    run(ctx, args) {
        const file = ctx.session.resolveAssetPath(args.file);
        let doc;
        try {
            doc = imaging.loadImage(file);
        } catch (err) {
            throw invalidArg(`cannot read image: ${err.message}`, { file });
        }
        const out = setDoc(ctx, doc, file);
        out.path = ctx.session.toProjectRelative(file);
        return out;
    },
});

def('image.info', {
    summary: 'Dimensions, layer list and pixel statistics: histogram, average color, content bounds, coverage, edge clipping, mirror symmetry and dominant colors.',
    args: {
        layer: { type: 'int', desc: 'Analyze this layer instead of the composite.' },
        spans: { type: 'bool', default: false, desc: 'Also report per-row and per-column content extents.' },
    },
    returns: '{width, height, path, layers, stats}',
    examples: ['cairn image.info', 'cairn image.info --layer 0 --spans'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        if (args.layer !== undefined) getLayer(ctx, args.layer);
        return Object.assign(describe(doc), {
            path: ctx.session._imagePath ? ctx.session.toProjectRelative(ctx.session._imagePath) : null,
            stats: imaging.analyze(doc, { layer: args.layer, spans: args.spans }),
        });
    },
});

def('image.preview', {
    summary: 'Render the image (or a layer) as text so its shape can be checked without opening a file.',
    args: {
        layer: { type: 'int', desc: 'Preview this layer instead of the composite.' },
        scale: { type: 'int', default: 1, desc: 'Average blocks of this many pixels into one cell.' },
        format: { type: 'enum', enum: ['ascii', 'blocks', 'hex'], default: 'ascii', desc: 'Cell representation.' },
        chars: { type: 'string', desc: 'Dark-to-light character ramp for the ascii/blocks formats.' },
        empty: { type: 'string', default: ' ', desc: 'Character for fully transparent cells.' },
    },
    returns: '{width, height, scale, cols, rows, format, lines}',
    examples: [
        'cairn image.preview --scale 4',
        'cairn image.preview --format blocks --scale 2',
        'cairn image.preview --format hex --scale 8',
    ],
    run(ctx, args) {
        const doc = getDoc(ctx);
        if (args.layer !== undefined) getLayer(ctx, args.layer);
        if (args.scale < 1 || args.scale > 64) throw invalidArg('--scale must be between 1 and 64');
        return imaging.previewText(doc, args);
    },
});

def('image.save', {
    summary: 'Write the composited image to a PNG file.',
    args: {
        out: { type: 'string', desc: 'Output path (default: the file it was opened from).' },
        layer: { type: 'int', desc: 'Save only this layer instead of the composite.' },
    },
    returns: '{path, width, height, bytes}',
    examples: ['cairn image.save --out Assets/Textures/wall.png'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        let target = args.out || ctx.session._imagePath;
        if (!target) throw usage('image.save needs --out (no source file to overwrite)');
        const file = ctx.session.resolveOutputPath(target);
        const saved = args.layer !== undefined
            ? imaging.saveImage({ width: doc.width, height: doc.height, layers: [getLayer(ctx, args.layer)] }, file)
            : imaging.saveImage(doc, file);
        ctx.session._imagePath = file;
        return Object.assign({ path: ctx.session.toProjectRelative(file) }, saved);
    },
});

// ------------------------------------------------------------------- layers
def('image.layer.add', {
    summary: 'Insert an empty layer above the given one (or on top).',
    args: {
        name: { type: 'string', default: 'Layer', desc: 'Layer name.' },
        index: { type: 'int', desc: 'Insert position (default: on top).' },
        fill: { type: 'color', desc: 'Fill color; omit for a transparent layer.' },
    },
    returns: '{index, name, layerCount}',
    examples: ['cairn image.layer.add --name Details'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const index = args.index === undefined ? doc.layers.length : Math.max(0, Math.min(doc.layers.length, args.index));
        doc.layers.splice(index, 0, imaging.newLayer(doc.width, doc.height, args.fill || null));
        if (args.name) doc.layers[index].name = args.name;
        return { index, name: doc.layers[index].name, layerCount: doc.layers.length };
    },
});

def('image.layer.duplicate', {
    summary: 'Copy a layer and insert the copy above it.',
    args: { index: { type: 'int', required: true, desc: 'Layer index.' } },
    returns: '{index, name, layerCount}',
    examples: ['cairn image.layer.duplicate --index 0'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const src = getLayer(ctx, args.index);
        const copy = {
            name: src.name + ' copy',
            visible: src.visible,
            opacity: src.opacity,
            data: Buffer.from(src.data),
        };
        doc.layers.splice(Number(args.index) + 1, 0, copy);
        return { index: Number(args.index) + 1, name: copy.name, layerCount: doc.layers.length };
    },
});

def('image.layer.delete', {
    summary: 'Delete a layer (the last remaining layer cannot be removed).',
    args: { index: { type: 'int', required: true, desc: 'Layer index.' } },
    returns: '{deleted, layerCount}',
    examples: ['cairn image.layer.delete --index 1'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        if (doc.layers.length <= 1) throw invalidArg('cannot delete the only layer');
        const [removed] = doc.layers.splice(Number(args.index), 1);
        return { deleted: removed.name, layerCount: doc.layers.length };
    },
});

def('image.layer.mergeDown', {
    summary: 'Composite a layer into the one below it (honouring opacity) and remove it.',
    args: { index: { type: 'int', required: true, desc: 'Layer index to merge (must be > 0).' } },
    returns: '{merged, into, layerCount}',
    examples: ['cairn image.layer.mergeDown --index 2'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const index = Number(args.index);
        if (index <= 0) throw invalidArg('nothing below layer 0 to merge into');
        getLayer(ctx, index);
        const merged = imaging.composite({
            width: doc.width,
            height: doc.height,
            layers: [doc.layers[index - 1], doc.layers[index]],
        });
        doc.layers[index - 1].data = merged;
        const [removed] = doc.layers.splice(index, 1);
        return { merged: removed.name, into: doc.layers[index - 1].name, layerCount: doc.layers.length };
    },
});

def('image.layer.set', {
    summary: 'Rename a layer or change its visibility/opacity.',
    args: {
        index: { type: 'int', required: true, desc: 'Layer index.' },
        name: { type: 'string', desc: 'New name.' },
        visible: { type: 'bool', desc: 'Visibility flag.' },
        opacity: { type: 'number', desc: 'Opacity 0-1.' },
    },
    returns: '{index, name, visible, opacity}',
    examples: ['cairn image.layer.set --index 0 --name Base --opacity 0.5'],
    run(ctx, args) {
        const layer = getLayer(ctx, args.index);
        if (args.name !== undefined) layer.name = args.name;
        if (args.visible !== undefined) layer.visible = args.visible;
        if (args.opacity !== undefined) {
            if (args.opacity < 0 || args.opacity > 1) throw invalidArg('--opacity must be between 0 and 1');
            layer.opacity = args.opacity;
        }
        return { index: Number(args.index), name: layer.name, visible: layer.visible, opacity: layer.opacity };
    },
});

def('image.layer.move', {
    summary: 'Reorder a layer within the stack (index 0 is the bottom).',
    args: {
        index: { type: 'int', required: true, desc: 'Current layer index.' },
        to: { type: 'int', required: true, desc: 'Destination index.' },
    },
    returns: '{from, to, layerCount}',
    examples: ['cairn image.layer.move --index 0 --to 2'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        getLayer(ctx, args.index);
        const to = Math.max(0, Math.min(doc.layers.length - 1, args.to));
        const [layer] = doc.layers.splice(Number(args.index), 1);
        doc.layers.splice(to, 0, layer);
        return { from: args.index, to, layerCount: doc.layers.length };
    },
});

def('image.layer.list', {
    summary: 'List layers with visibility and opacity.',
    args: {},
    returns: '{width, height, layers:[{index, name, visible, opacity}]}',
    examples: ['cairn image.layer.list'],
    run(ctx) {
        return describe(getDoc(ctx));
    },
});

// ------------------------------------------------------------------ drawing
const drawArgs = {
    layer: { type: 'int', desc: 'Target layer index (default: the top layer).' },
    color: { type: 'color', default: '#ffffff', desc: 'Stroke/fill color (hex or [r,g,b,a]).' },
    opacity: { type: 'number', default: 1, desc: 'Alpha multiplier 0-1.' },
    brush: { type: 'number', default: 1, desc: 'Stroke width / brush diameter in pixels.' },
    hardness: { type: 'number', default: 0.8, desc: 'Brush hardness 0-1 (1 = hard edge).' },
};

// `--mirror` draws the shape and its reflection about the canvas centre line,
// which is how a symmetric character gets painted from one half.
const mirrorArg = {
    mirror: {
        type: 'enum', enum: ['none', 'x', 'y', 'both'], default: 'none',
        desc: 'Also paint the reflection about the canvas centre line (x = left/right, both = 180°).',
    },
};

// Shared by the shape commands: `--filled` fills the interior, `--stroke`
// adds an outline drawn *outside* the boundary (so it never eats the fill),
// and `--strokeColor` lets fill and outline differ (e.g. a gold coin with a
// dark rim) in a single call.
const shapeArgs = Object.assign({}, drawArgs, mirrorArg, {
    filled: { type: 'bool', default: false, desc: 'Fill the interior.' },
    stroke: { type: 'number', default: 0, desc: 'Outline width in pixels, drawn outside the shape (0 = none).' },
    strokeColor: { type: 'color', desc: 'Outline color (default: the fill color).' },
});

// Runs a draw operation. `fn(pass, data, w, h, layer)` receives the argument
// set to use -- with `--mirror` the operation runs a second time on
// `imaging.mirrorShapeArgs`'s reflected geometry and the returned pixel count
// is the first pass's. `options.box` marks commands whose x/y is the corner of
// a width/height box rather than a point.
function draw(ctx, args, fn, options = {}) {
    if (args.stroke !== undefined && args.stroke < 0) throw invalidArg('--stroke must be 0 or greater');
    const layer = activeLayer(ctx, args);
    const doc = ctx.session._image;
    const passes = [args];
    if (args.mirror && args.mirror !== 'none') {
        passes.push(imaging.mirrorShapeArgs(args, doc.width, doc.height, !!options.box));
    }
    let count;
    for (const pass of passes) {
        const result = fn(pass, layer.data, doc.width, doc.height, layer);
        if (pass === args) count = result;
    }
    const out = {
        layer: doc.layers.indexOf(layer),
        pixels: count === undefined ? null : count,
        width: doc.width,
        height: doc.height,
    };
    if (passes.length > 1) out.mirrored = args.mirror;
    return out;
}

def('image.draw.rect', {
    summary: 'Draw a rectangle outline or a filled rectangle.',
    args: Object.assign({}, shapeArgs, {
        x: { type: 'number', required: true, desc: 'Left X.' },
        y: { type: 'number', required: true, desc: 'Top Y.' },
        width: { type: 'number', required: true, desc: 'Rectangle width.' },
        height: { type: 'number', required: true, desc: 'Rectangle height.' },
    }),
    returns: '{layer, pixels, mirrored?}',
    examples: [
        'cairn image.draw.rect --x 8 --y 8 --width 64 --height 64 --color "#ff8800" --filled',
        'cairn image.draw.rect --x 8 --y 8 --width 48 --height 48 --filled --stroke 2 --color "#ffd700" --strokeColor "#4a3200"',
    ],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            if (a.filled) imaging.fillRect(data, w, h, a.x, a.y, a.width, a.height, a.color, a.opacity);
            if (a.stroke > 0) imaging.strokeRectRing(data, w, h, a.x, a.y, a.width, a.height, a.stroke, a.strokeColor || a.color, a.opacity);
            else if (!a.filled) imaging.strokeRect(data, w, h, a.x, a.y, a.x + a.width, a.y + a.height, a.color, a.opacity, a.brush);
        }, { box: true });
    },
});

def('image.draw.line', {
    summary: 'Draw a line segment.',
    args: Object.assign({}, drawArgs, mirrorArg, {
        x1: { type: 'number', required: true, desc: 'Start X.' },
        y1: { type: 'number', required: true, desc: 'Start Y.' },
        x2: { type: 'number', required: true, desc: 'End X.' },
        y2: { type: 'number', required: true, desc: 'End Y.' },
    }),
    returns: '{layer, pixels, mirrored?}',
    examples: ['cairn image.draw.line --x1 0 --y1 0 --x2 127 --y2 127 --color white --brush 3'],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            imaging.strokeLine(data, w, h, a.x1, a.y1, a.x2, a.y2, a.color, a.opacity, a.brush);
        });
    },
});

def('image.draw.ellipse', {
    summary: 'Draw an ellipse outline or a filled ellipse.',
    args: Object.assign({}, shapeArgs, {
        cx: { type: 'number', required: true, desc: 'Center X.' },
        cy: { type: 'number', required: true, desc: 'Center Y.' },
        rx: { type: 'number', required: true, desc: 'Horizontal radius.' },
        ry: { type: 'number', required: true, desc: 'Vertical radius.' },
    }),
    returns: '{layer, pixels, mirrored?}',
    examples: [
        'cairn image.draw.ellipse --cx 64 --cy 64 --rx 40 --ry 24 --filled --color "#3af"',
        'cairn image.draw.ellipse --cx 64 --cy 64 --rx 30 --ry 30 --filled --stroke 3 --color "#3af" --strokeColor "#124"',
    ],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            if (a.filled) imaging.fillEllipse(data, w, h, a.cx, a.cy, a.rx, a.ry, a.color, a.opacity);
            if (a.stroke > 0) imaging.strokeEllipseRing(data, w, h, a.cx, a.cy, a.rx, a.ry, a.stroke, a.strokeColor || a.color, a.opacity);
            else if (!a.filled) imaging.strokeEllipse(data, w, h, a.cx, a.cy, a.rx, a.ry, a.color, a.opacity, a.brush);
        });
    },
});

function pathPoints(a) {
    const pts = imaging.parsePoints(a.points);
    if (pts) return pts;
    return a.sides === undefined ? null : imaging.regularPolygonPoints(a);
}

function pathPointsError() {
    return invalidArg('image.draw.path needs --points or --sides (at least 2 vertices)', {
        hint: 'e.g. --points "10,10 50,10 30,50" or --sides 6 --cx 32 --cy 32 --radius 24',
    });
}

def('image.draw.path', {
    summary: 'Draw a closed polygon, an open polyline, or a regular polygon/star.',
    args: Object.assign({}, shapeArgs, {
        points: { type: 'json', desc: 'Vertices: "x,y x,y …", "x,y;x,y", or JSON [[x,y],…].' },
        closed: { type: 'bool', default: true, desc: 'Stroke the closing edge (an open polyline when false).' },
        sides: { type: 'int', desc: 'Build a regular polygon with this many sides instead of --points.' },
        cx: { type: 'number', desc: 'Center X for --sides.' },
        cy: { type: 'number', desc: 'Center Y for --sides.' },
        radius: { type: 'number', desc: 'Outer radius for --sides.' },
        rotation: { type: 'number', default: 0, desc: 'Rotation in degrees for --sides (0 points up).' },
        inner: { type: 'number', desc: 'Inner radius ratio 0-1; > 0 makes a star.' },
    }),
    returns: '{layer, pixels, mirrored?}',
    examples: [
        'cairn image.draw.path --points "32,4 60,20 60,52 32,60 4,52 4,20" --filled --color "#4a90d9"',
        'cairn image.draw.path --sides 5 --inner 0.45 --cx 32 --cy 32 --radius 28 --filled --stroke 2 --color "#ffd700" --strokeColor "#4a3200"',
        'cairn image.draw.path --points "8,56 24,16 40,56" --closed false --stroke 3 --color white',
        'cairn image.draw.path --points "4,40 12,4 20,40" --filled --color "#3a7de0" --mirror x',
    ],
    run(ctx, args) {
        if (!pathPoints(args)) throw pathPointsError();
        return draw(ctx, args, (a, data, w, h) => {
            const points = pathPoints(a);
            if (!points) throw pathPointsError();
            if (a.filled) imaging.fillPolygon(data, w, h, points, a.color, a.opacity);
            if (a.stroke > 0) {
                if (a.closed && points.length >= 3) {
                    imaging.strokePolygonRing(data, w, h, points, a.stroke, a.strokeColor || a.color, a.opacity);
                } else {
                    imaging.strokePolygon(data, w, h, points, a.strokeColor || a.color, a.opacity, a.stroke, a.closed);
                }
            } else if (!a.filled) {
                imaging.strokePolygon(data, w, h, points, a.color, a.opacity, a.brush, a.closed);
            }
            return points.length;
        });
    },
});

def('image.draw.brush', {
    summary: 'Paint a brush stroke along a path (single point when no end is given).',
    args: Object.assign({}, drawArgs, mirrorArg, {
        x: { type: 'number', required: true, desc: 'Start X.' },
        y: { type: 'number', required: true, desc: 'Start Y.' },
        x2: { type: 'number', desc: 'End X for a stroke.' },
        y2: { type: 'number', desc: 'End Y for a stroke.' },
        erase: { type: 'bool', default: false, desc: 'Erase alpha instead of painting.' },
    }),
    returns: '{layer, pixels, mirrored?}',
    examples: [
        'cairn image.draw.brush --x 20 --y 20 --brush 12 --color "#e8e8ee"',
        'cairn image.draw.brush --x 10 --y 10 --x2 90 --y2 90 --brush 6 --color red',
        'cairn image.draw.brush --x 16 --y 30 --x2 16 --y2 60 --brush 5 --mirror x',
    ],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            const radius = Math.max(0.5, a.brush / 2);
            const points = a.x2 === undefined || a.y2 === undefined
                ? [[a.x, a.y]]
                : interpolate(a.x, a.y, a.x2, a.y2, radius);
            for (const [px, py] of points) {
                if (a.erase) imaging.stampEraser(data, w, h, px, py, radius, a.opacity);
                else imaging.stampDisc(data, w, h, px, py, radius, a.color, a.opacity, a.hardness);
            }
            return points.length;
        });
    },
});

def('image.draw.fill', {
    summary: 'Flood fill the contiguous region around a pixel (tolerance matched).',
    args: Object.assign({}, drawArgs, {
        x: { type: 'number', required: true, desc: 'Seed X.' },
        y: { type: 'number', required: true, desc: 'Seed Y.' },
        tolerance: { type: 'int', default: 24, desc: 'Per-channel colour tolerance 0-255.' },
    }),
    returns: '{layer, pixels}',
    examples: ['cairn image.draw.fill --x 64 --y 64 --color "#2288ff"'],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            imaging.floodFill(data, w, h, a.x, a.y, a.color, a.opacity, a.tolerance);
        });
    },
});

// Deterministic noise and gradients are region fills with a colour ramp, not
// brush strokes: `--from` is the low end of the ramp and `--to` the high end,
// and the optional x/y/width/height region defaults to the whole canvas.
const rampArgs = {
    layer: { type: 'int', desc: 'Target layer index (default: the top layer).' },
    from: { type: 'color', default: '#000000', desc: 'Colour at the low end of the ramp.' },
    to: { type: 'color', default: '#ffffff', desc: 'Colour at the high end of the ramp.' },
    opacity: { type: 'number', default: 1, desc: 'Alpha multiplier 0-1.' },
    x: { type: 'number', desc: 'Region left (default: the whole canvas).' },
    y: { type: 'number', desc: 'Region top (default: the whole canvas).' },
    width: { type: 'number', desc: 'Region width (default: the whole canvas).' },
    height: { type: 'number', desc: 'Region height (default: the whole canvas).' },
};

def('image.draw.noise', {
    summary: 'Fill a region with seeded, reproducible noise (value, white or fractal).',
    args: Object.assign({}, rampArgs, {
        kind: { type: 'enum', enum: ['value', 'white', 'fractal'], default: 'value', desc: 'value = smooth blobs, white = per-pixel grain, fractal = octave sum.' },
        seed: { type: 'int', default: 0, desc: 'Seed: the same seed always paints the same pixels.' },
        cell: { type: 'number', default: 16, desc: 'Lattice cell size in pixels (value/fractal).' },
        octaves: { type: 'int', default: 4, desc: 'Octaves summed by --kind fractal.' },
        lacunarity: { type: 'number', default: 2, desc: 'Cell shrink per octave.' },
        gain: { type: 'number', default: 0.5, desc: 'Amplitude per octave 0-1.' },
        tile: { type: 'bool', default: false, desc: 'Wrap the lattice so the field is exactly periodic (a seamless tile).' },
    }),
    returns: '{layer, painted, kind, seed, tile, width, height}',
    examples: [
        'cairn image.draw.noise --kind fractal --seed 7 --cell 24 --from "#2a1a0a" --to "#a67c4a"',
        'cairn image.draw.noise --kind white --seed 3 --from "#000000" --to "#ffffff" --opacity 0.25',
        'cairn image.draw.noise --kind value --seed 5 --cell 16 --tile --from "#12384a" --to "#9ccfe8"',
    ],
    run(ctx, args) {
        if (!(args.cell > 0)) throw invalidArg('--cell must be greater than 0', { cell: args.cell });
        if (args.octaves < 1) throw invalidArg('--octaves must be 1 or greater', { octaves: args.octaves });
        if (args.lacunarity < 1) throw invalidArg('--lacunarity must be 1 or greater', { lacunarity: args.lacunarity });
        if (args.gain < 0 || args.gain > 1) throw invalidArg('--gain must be between 0 and 1', { gain: args.gain });
        const layer = activeLayer(ctx, args);
        const doc = ctx.session._image;
        const painted = imaging.drawNoise(layer.data, doc.width, doc.height, args);
        return {
            layer: doc.layers.indexOf(layer),
            painted,
            kind: args.kind,
            seed: args.seed,
            tile: args.tile && args.kind !== 'white',
            width: doc.width,
            height: doc.height,
        };
    },
});

def('image.draw.gradient', {
    summary: 'Fill a region with a linear or radial colour ramp.',
    args: Object.assign({}, rampArgs, {
        kind: { type: 'enum', enum: ['linear', 'radial'], default: 'linear', desc: 'linear along an axis, or radial from a centre outwards.' },
        angle: { type: 'number', default: 90, desc: 'Linear direction in degrees (0 = left to right, 90 = top to bottom).' },
        x1: { type: 'number', desc: 'Linear start X (overrides --angle/--y1/--x2/--y2).' },
        y1: { type: 'number', desc: 'Linear start Y.' },
        x2: { type: 'number', desc: 'Linear end X.' },
        y2: { type: 'number', desc: 'Linear end Y.' },
        cx: { type: 'number', desc: 'Radial centre X (default: the canvas centre).' },
        cy: { type: 'number', desc: 'Radial centre Y.' },
        radius: { type: 'number', desc: 'Radial radius in pixels (default: half the smaller side).' },
    }),
    returns: '{layer, painted, kind, width, height}',
    examples: [
        'cairn image.draw.gradient --kind linear --angle 90 --from "#0b1e3a" --to "#7fb2e5"',
        'cairn image.draw.gradient --kind radial --cx 32 --cy 32 --radius 30 --from "#ffffff" --to "#ffd700"',
        'cairn image.draw.gradient --x1 0 --y1 0 --x2 63 --y2 63 --from "#000000" --to "#ffffff"',
    ],
    run(ctx, args) {
        const doc = getDoc(ctx);
        if (args.kind === 'radial') {
            const radius = args.radius === undefined ? Math.min(doc.width, doc.height) / 2 : args.radius;
            if (!(radius > 0)) throw invalidArg('--radius must be greater than 0', { radius: args.radius });
        } else if (args.x1 !== undefined || args.x2 !== undefined) {
            const x1 = args.x1 === undefined ? 0 : args.x1, y1 = args.y1 === undefined ? 0 : args.y1;
            const x2 = args.x2 === undefined ? doc.width : args.x2;
            const y2 = args.y2 === undefined ? doc.height : args.y2;
            if (x1 === x2 && y1 === y2) throw invalidArg('the gradient start and end must differ');
        }
        const layer = activeLayer(ctx, args);
        const painted = imaging.drawGradient(layer.data, doc.width, doc.height, args);
        return { layer: doc.layers.indexOf(layer), painted, kind: args.kind, width: doc.width, height: doc.height };
    },
});

def('image.pixel', {
    summary: 'Sample a pixel from the active layer or the composite.',
    args: {
        x: { type: 'int', required: true, desc: 'X in pixels.' },
        y: { type: 'int', required: true, desc: 'Y in pixels (origin top-left).' },
        layer: { type: 'int', desc: 'Sample this layer instead of the composite.' },
    },
    returns: '{x, y, r, g, b, a, hex}',
    examples: ['cairn image.pixel --x 0 --y 0'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const data = args.layer === undefined
            ? imaging.composite(doc)
            : getLayer(ctx, args.layer).data;
        return imaging.samplePixel(data, doc.width, doc.height, args.x, args.y);
    },
});

def('image.replaceColor', {
    summary: 'Recolor every pixel of a layer whose color matches within a tolerance.',
    args: Object.assign({}, drawArgs, {
        from: { type: 'color', required: true, desc: 'Color to replace.' },
        tolerance: { type: 'int', default: 8, desc: 'Per-channel tolerance 0-255.' },
    }),
    returns: '{layer, pixels}',
    examples: ['cairn image.replaceColor --from "#ff0000" --color "#00ff00"'],
    run(ctx, args) {
        return draw(ctx, args, (a, data, w, h) => {
            const target = a.from.map(v => v * 255);
            let changed = 0;
            for (let p = 0; p < w * h; ++p) {
                const o = p * 4;
                if (Math.abs(data[o] - target[0]) > a.tolerance
                    || Math.abs(data[o + 1] - target[1]) > a.tolerance
                    || Math.abs(data[o + 2] - target[2]) > a.tolerance
                    || Math.abs(data[o + 3] - target[3]) > a.tolerance) continue;
                data[o] = Math.round(a.color[0] * 255);
                data[o + 1] = Math.round(a.color[1] * 255);
                data[o + 2] = Math.round(a.color[2] * 255);
                data[o + 3] = Math.round(a.color[3] * 255);
                changed++;
            }
            return changed;
        });
    },
});

// An explicit palette: "#112233,#445566", a "1,2,3 4,5,6" list, or a JSON array
// of those strings / [r,g,b] triples.
function parsePalette(value) {
    const items = Array.isArray(value) ? value : String(value).split(/[,\s;]+/).filter(Boolean);
    if (!items.length) throw invalidArg('--palette needs at least one colour');
    return items.map((item, i) => {
        const rgba = imaging.parseColor(item);
        if (!rgba) throw invalidArg(`--palette entry ${i + 1} is not a colour`, { entry: item });
        return { r: Math.round(rgba[0] * 255), g: Math.round(rgba[1] * 255), b: Math.round(rgba[2] * 255) };
    });
}

function paletteHex(entry) {
    return imaging.rgbaToHex([entry.r / 255, entry.g / 255, entry.b / 255]);
}

def('image.palette', {
    summary: 'Quantize a layer to a palette (median cut), with optional dithering, or report it.',
    args: {
        colors: { type: 'int', default: 16, desc: 'Palette size for the automatic median cut (1-256).' },
        palette: { type: 'json', desc: 'Explicit palette instead: "#112233,#445566" or a JSON array.' },
        dither: { type: 'enum', enum: ['none', 'ordered', 'floyd'], default: 'none', desc: 'none, an ordered Bayer threshold, or Floyd-Steinberg.' },
        layer: { type: 'int', desc: 'Target layer (default: the top layer).' },
        list: { type: 'bool', default: false, desc: 'Report the palette and how many pixels use each entry instead of applying it.' },
    },
    returns: '--list: {layer, colors, palette:[{hex,count}], total}; else {layer, colors, dither, palette:[hex], changed}',
    examples: [
        'cairn image.palette --colors 8 --dither floyd',
        'cairn image.palette --palette "#1a1c2c,#5d275d,#b13e53,#ef7d57" --dither ordered',
        'cairn image.palette --colors 16 --list',
    ],
    run(ctx, args) {
        if (args.colors < 1 || args.colors > 256) {
            throw invalidArg('--colors must be between 1 and 256', { colors: args.colors });
        }
        // Parsed before the document is needed, so a bad --palette reports
        // itself whether or not an image is loaded.
        const explicit = args.palette === undefined ? null : parsePalette(args.palette);
        const layer = activeLayer(ctx, args);
        const doc = ctx.session._image;
        const palette = explicit || imaging.paletteFromImage(layer.data, doc.width, doc.height, args.colors);
        const layerIndex = doc.layers.indexOf(layer);
        if (args.list) {
            // The counts are where each pixel actually lands, so they line up
            // with what a quantize would do (dithering aside).
            const counts = imaging.paletteCounts(layer.data, doc.width, doc.height, palette);
            return {
                layer: layerIndex,
                colors: palette.length,
                palette: palette.map((entry, i) => ({ hex: paletteHex(entry), count: counts[i] })),
                total: counts.reduce((a, b) => a + b, 0),
                width: doc.width,
                height: doc.height,
            };
        }
        const changed = imaging.quantizeLayer(layer.data, doc.width, doc.height, { palette, dither: args.dither });
        return {
            layer: layerIndex,
            colors: palette.length,
            dither: args.dither,
            palette: palette.map(paletteHex),
            changed,
            width: doc.width,
            height: doc.height,
        };
    },
});

def('image.flip', {
    summary: 'Mirror the document left/right or top/bottom.',
    args: {
        axis: { type: 'enum', enum: ['x', 'y', 'both'], default: 'x', desc: 'x = left/right, y = top/bottom.' },
        layer: { type: 'int', desc: 'Flip only this layer instead of every layer.' },
    },
    returns: '{width, height, axis, layers}',
    examples: [
        'cairn image.flip --axis x',
        'cairn image.flip --axis y --layer 2',
    ],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const targets = args.layer === undefined ? doc.layers : [getLayer(ctx, args.layer)];
        for (const layer of targets) imaging.flipLayerData(layer.data, doc.width, doc.height, args.axis);
        return { width: doc.width, height: doc.height, axis: args.axis, layers: targets.length };
    },
});

def('image.rotate', {
    summary: 'Rotate every layer clockwise by a multiple of 90 degrees.',
    args: {
        degrees: { type: 'number', required: true, desc: 'Clockwise rotation; must be a multiple of 90 (90/270 swap the canvas width and height).' },
    },
    returns: '{width, height, degrees}',
    examples: ['cairn image.rotate --degrees 90', 'cairn image.rotate --degrees 270'],
    run(ctx, args) {
        if (Math.abs(args.degrees % 90) > 1e-9) {
            throw invalidArg('--degrees must be a multiple of 90', { degrees: args.degrees });
        }
        const doc = getDoc(ctx);
        const rotated = doc.layers.map(layer => imaging.rotateRgba90(layer.data, doc.width, doc.height, args.degrees / 90));
        doc.layers.forEach((layer, i) => { layer.data = rotated[i].data; });
        doc.width = rotated[0].width;
        doc.height = rotated[0].height;
        return { width: doc.width, height: doc.height, degrees: args.degrees };
    },
});

def('image.crop', {
    summary: 'Crop every layer to a rectangle (clamped to the canvas).',
    args: {
        x: { type: 'int', required: true, desc: 'Left edge.' },
        y: { type: 'int', required: true, desc: 'Top edge.' },
        width: { type: 'int', required: true, desc: 'Crop width.' },
        height: { type: 'int', required: true, desc: 'Crop height.' },
    },
    returns: '{x, y, width, height}',
    examples: ['cairn image.crop --x 16 --y 16 --width 32 --height 32'],
    run(ctx, args) {
        if (args.width < 1 || args.height < 1) throw invalidArg('crop size must be at least 1x1');
        const doc = getDoc(ctx);
        const x = imaging.clamp(args.x, 0, doc.width - 1);
        const y = imaging.clamp(args.y, 0, doc.height - 1);
        const width = Math.min(args.width, doc.width - x);
        const height = Math.min(args.height, doc.height - y);
        for (const layer of doc.layers) {
            layer.data = imaging.cropRgba(layer.data, doc.width, doc.height, x, y, width, height);
        }
        doc.width = width;
        doc.height = height;
        return { x, y, width, height };
    },
});

def('image.stamp', {
    summary: 'Composite an image file or another layer onto a layer at an offset.',
    args: {
        file: { type: 'string', desc: 'PNG/BMP file to stamp (project-relative or absolute).' },
        from: { type: 'int', desc: 'Source layer index inside this document.' },
        layer: { type: 'int', desc: 'Target layer (default: the top layer).' },
        x: { type: 'number', default: 0, desc: 'Destination left.' },
        y: { type: 'number', default: 0, desc: 'Destination top.' },
        scale: { type: 'number', default: 1, desc: 'Scale the source before stamping (nearest neighbour).' },
        opacity: { type: 'number', default: 1, desc: 'Alpha multiplier 0-1.' },
        blend: { type: 'enum', enum: ['over', 'replace'], default: 'over', desc: 'Source-over, or replace that also copies alpha.' },
    },
    returns: '{layer, painted, source, width, height}',
    examples: [
        'cairn image.stamp --file Assets/Sprites/coin.png --x 64 --y 32',
        'cairn image.stamp --from 0 --x 16 --y 16 --scale 0.5 --opacity 0.6',
        'cairn image.stamp --file Assets/Sprites/tile.png --x 0 --y 0 --blend replace',
    ],
    run(ctx, args) {
        if (!args.file && args.from === undefined) throw invalidArg('image.stamp needs --file or --from');
        if (args.file && args.from !== undefined) throw invalidArg('give either --file or --from, not both');
        if (args.scale <= 0) throw invalidArg('--scale must be greater than 0', { scale: args.scale });
        const doc = getDoc(ctx);
        let src;
        if (args.file) {
            const file = ctx.session.resolveAssetPath(args.file);
            let image;
            try {
                image = imaging.loadImage(file);
            } catch (err) {
                throw invalidArg(`cannot read image: ${err.message}`, { file });
            }
            src = { width: image.width, height: image.height, data: image.layers[0].data };
        } else {
            const layer = getLayer(ctx, args.from);
            src = { width: doc.width, height: doc.height, data: Buffer.from(layer.data) };
        }
        if (args.scale !== 1) {
            const width = Math.max(1, Math.round(src.width * args.scale));
            const height = Math.max(1, Math.round(src.height * args.scale));
            src = { width, height, data: imaging.resizeRgba(src.data, src.width, src.height, width, height) };
        }
        const target = activeLayer(ctx, args);
        const painted = imaging.stampImage(target.data, doc.width, doc.height, src, {
            x: args.x,
            y: args.y,
            alpha: args.opacity,
            blend: args.blend,
        });
        return {
            layer: doc.layers.indexOf(target),
            painted,
            source: { width: src.width, height: src.height },
            width: doc.width,
            height: doc.height,
        };
    },
});

def('image.normalMap', {
    summary: 'Generate a tangent-space normal map from the composite height field and add it as a new layer.',
    args: {
        source: { type: 'enum', enum: ['luma', 'alpha'], default: 'luma', desc: 'Height source channel.' },
        strength: { type: 'number', default: 6, desc: 'Sobel strength (1-20).' },
        invertX: { type: 'bool', default: false, desc: 'Flip the X component.' },
        invertY: { type: 'bool', default: false, desc: 'Flip the Y component.' },
        out: { type: 'string', desc: 'Also write the normal map to this PNG path.' },
        name: { type: 'string', default: 'Normal Map', desc: 'Name of the created layer.' },
    },
    returns: '{width, height, layerIndex, out?}',
    examples: ['cairn image.normalMap --source luma --strength 8 --out Assets/Textures/wall_n.png'],
    run(ctx, args) {
        const doc = getDoc(ctx);
        const data = imaging.normalMapFromHeight(imaging.composite(doc), doc.width, doc.height, args);
        const layer = { name: args.name, visible: true, opacity: 1, data };
        doc.layers.push(layer);
        const out = {
            width: doc.width,
            height: doc.height,
            layerIndex: doc.layers.length - 1,
            layerCount: doc.layers.length,
        };
        if (args.out) {
            const file = ctx.session.resolveOutputPath(args.out);
            const single = { width: doc.width, height: doc.height, layers: [layer] };
            imaging.saveImage(single, file);
            out.out = ctx.session.toProjectRelative(file);
        }
        return out;
    },
});

def('image.flatten', {
    summary: 'Collapse all visible layers into a single layer.',
    args: {},
    returns: '{width, height, layerCount}',
    examples: ['cairn image.flatten'],
    run(ctx) {
        const doc = getDoc(ctx);
        doc.layers = [{ name: 'Background', visible: true, opacity: 1, data: imaging.composite(doc) }];
        return { width: doc.width, height: doc.height, layerCount: 1 };
    },
});

def('image.resize', {
    summary: 'Resample every layer to new dimensions (nearest neighbour).',
    args: {
        width: { type: 'int', required: true, desc: 'New width in pixels.' },
        height: { type: 'int', required: true, desc: 'New height in pixels.' },
    },
    returns: '{width, height}',
    examples: ['cairn image.resize --width 512 --height 512'],
    run(ctx, args) {
        if (args.width < 1 || args.height < 1 || args.width > 4096 || args.height > 4096) {
            throw invalidArg('image size must be between 1 and 4096 pixels');
        }
        const doc = imaging.resizeDoc(getDoc(ctx), args.width, args.height);
        return { width: doc.width, height: doc.height };
    },
});

def('image.color', {
    summary: 'Convert colors between hex, rgb 0-255, rgb 0-1 and hsv.',
    args: {
        value: { type: 'color', required: true, desc: 'Color as hex, name, "r,g,b,a" or [r,g,b,a] (0-255 or 0-1).' },
        to: { type: 'enum', enum: ['hex', 'rgb255', 'rgb01', 'hsv'], default: 'hex', desc: 'Target representation.' },
    },
    returns: '{hex, rgb255, rgb01, hsv}',
    examples: ['cairn image.color --value \'[255,128,0]\'', 'cairn image.color --value \'[1,0.5,0]\' --to hsv'],
    run(ctx, args) {
        const rgba = imaging.parseColor(args.value);
        if (!rgba) throw invalidArg('could not parse color');
        const rgb255 = rgba.slice(0, 3).map(v => Math.round(v * 255));
        const hsv = imaging.rgbToHsv(rgb255[0], rgb255[1], rgb255[2]);
        return {
            hex: imaging.rgbaToHex(rgba),
            rgb255,
            rgb01: rgba.slice(0, 3).map(v => Math.round(v * 1000) / 1000),
            alpha: Math.round(rgba[3] * 1000) / 1000,
            hsv: {
                h: Math.round(hsv.h * 1000) / 1000,
                s: Math.round(hsv.s * 1000) / 1000,
                v: Math.round(hsv.v * 1000) / 1000,
            },
        };
    },
});

function interpolate(x1, y1, x2, y2, radius) {
    const dist = Math.hypot(x2 - x1, y2 - y1);
    const steps = Math.max(1, Math.ceil(dist / Math.max(0.25, radius * 0.25)));
    const out = [];
    for (let i = 0; i <= steps; ++i) {
        const t = i / steps;
        out.push([x1 + (x2 - x1) * t, y1 + (y2 - y1) * t]);
    }
    return out;
}

module.exports = {};
