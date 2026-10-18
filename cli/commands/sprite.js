// Cairn CLI - sprite sheet inspection: the cells a SpriteAnimator slices a
// texture into, the cells each clip plays with their times, and a preview PNG of
// that slicing. The numbers come from the shared sheet model
// (editor/main/sheet.js) that the editor's 2D panel draws, so an agent can ask
// the same questions the panel answers without opening a window.
'use strict';

const { def } = require('../lib/registry');
const sheet = require('../lib/sheet');
const imaging = require('../lib/imaging');
const { invalidArg, notFound } = require('../lib/errors');
const { requireEntity } = require('./entity');

// Preview marks, chosen to read on the flat colours sheets are drawn with: the
// slicing in cyan, the cells the clip plays boxed in magenta, and the cell the
// animator draws next lit up.
const GRID_RGBA = imaging.parseColor('#00ffff');
const CLIP_RGBA = imaging.parseColor('#ff00ff');
const FRAME_RGBA = [1, 1, 1, 1];
const FRAME_ALPHA = 0.4;
const PREVIEW_TARGET = 256;   // what the automatic zoom aims the sheet at, in pixels
const PREVIEW_MAX_SCALE = 16;

function componentProps(components, type) {
    const found = (components || []).find(c => c && c.type === type);
    return found ? (found.props || {}) : null;
}

function uvRectOf(value) {
    if (!Array.isArray(value) || value.length < 4) return { x: 0, y: 0, w: 1, h: 1 };
    const n = (v, def) => (typeof v === 'number' && isFinite(v) ? v : def);
    return { x: n(value[0], 0), y: n(value[1], 0), w: n(value[2], 1), h: n(value[3], 1) };
}

// The sheet, scaled up by whole pixels so the grid lines stay on texel edges.
// Lines sit on cell boundaries and the outermost ones are kept a pixel inside
// the image, so a sheet's own border is drawn rather than clipped away.
function drawPreview(file, doc, scale, grid, timeline, frame, drawGrid) {
    const pw = doc.width * scale;
    const ph = doc.height * scale;
    const px = imaging.resizeRgba(imaging.composite(doc), doc.width, doc.height, pw, ph);

    const horizontal = new Set();
    const vertical = new Set();
    for (const cell of grid.cells) {
        horizontal.add(cell.y);
        horizontal.add(cell.y + cell.h);
        vertical.add(cell.x);
        vertical.add(cell.x + cell.w);
    }
    const across = [...vertical].map(x => Math.min(pw - 1, x * scale));
    const down = [...horizontal].map(y => Math.min(ph - 1, y * scale));

    const lit = grid.cells[frame];
    if (lit) {
        imaging.fillRect(px, pw, ph, lit.x * scale, lit.y * scale, lit.w * scale, lit.h * scale,
            FRAME_RGBA, FRAME_ALPHA);
    }
    if (drawGrid) {
        for (const x of across) imaging.fillRect(px, pw, ph, x, 0, 1, ph, GRID_RGBA);
        for (const y of down) imaging.fillRect(px, pw, ph, 0, y, pw, 1, GRID_RGBA);
    }
    // The clip's own box, over the slicing, so a cell it plays reads as a
    // different frame from one it does not. Lines are laid down a pixel at a
    // time: a one-pixel stroke through the shared brush core has no coverage at
    // all (its disc falloff rounds a radius of a half to nothing).
    for (const cell of timeline.cells) {
        const x0 = Math.min(pw - 1, cell.x * scale);
        const x1 = Math.min(pw - 1, (cell.x + cell.w) * scale);
        const y0 = Math.min(ph - 1, cell.y * scale);
        const y1 = Math.min(ph - 1, (cell.y + cell.h) * scale);
        imaging.fillRect(px, pw, ph, x0, y0, x1 - x0 + 1, 1, CLIP_RGBA);
        imaging.fillRect(px, pw, ph, x0, y1, x1 - x0 + 1, 1, CLIP_RGBA);
        imaging.fillRect(px, pw, ph, x0, y0, 1, y1 - y0 + 1, CLIP_RGBA);
        imaging.fillRect(px, pw, ph, x1, y0, 1, y1 - y0 + 1, CLIP_RGBA);
    }

    const out = imaging.createDoc(pw, ph, { background: null });
    out.layers.push({ name: 'Sheet', visible: true, opacity: 1, data: px });
    return imaging.saveImage(out, file).path;
}

def('sprite.sheet', {
    summary: 'Describe a sprite sheet the way a SpriteAnimator slices it: every cell in texels, ' +
        'the cells a clip plays with their times, and optionally a preview PNG.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity carrying the SpriteRenderer (and the SpriteAnimator) to describe.' },
        clip: { type: 'string', desc: 'Describe this clip instead of the animator\'s current one.' },
        frame: { type: 'int', desc: 'Report and highlight this cell instead of the animator\'s own frame.' },
        scale: { type: 'int', default: 0, desc: 'Preview zoom; 0 picks the whole-number zoom that fits the sheet in about 256 pixels.' },
        output: { type: 'path', desc: 'Write a preview PNG here: the sheet scaled up, the slicing in cyan, the clip\'s cells boxed in magenta and the current cell lit.' },
        grid: { type: 'bool', default: true, desc: 'Draw every cell\'s frame in the preview.' },
    },
    returns: '{id, name, texture, textureSource, width, height, columns, rows, rect, region, cell, count, cells, ' +
        'clip, clips, frame, preview}',
    needsEngine: true,
    needsProject: true,
    notes: 'Cells are numbered row-major from the uvRect\'s top-left corner and are the order ' +
        'SpriteAnimator frames and clips use. A clip\'s cell times follow the engine: its own ' +
        'durations entry when that is a positive number, 1 / fps otherwise. --format text lays ' +
        'the cells out as tables.',
    examples: [
        'cairn --project ./game --scene Assets/Scenes/Main.cscene sprite.sheet --id 5',
        'cairn --project ./game --scene Assets/Scenes/Main.cscene sprite.sheet --id 5 --clip walk --format text',
        'cairn --project ./game --scene Assets/Scenes/Main.cscene sprite.sheet --id 5 --output /tmp/sheet.png',
    ],
    run(ctx, args) {
        const entity = requireEntity(ctx, args.id);
        const components = entity.components || [];
        const sprite = componentProps(components, 'SpriteRenderer');
        if (!sprite) {
            throw notFound(`entity ${args.id} has no SpriteRenderer to slice`, {
                available: components.map(c => c.type),
            });
        }
        const animator = componentProps(components, 'SpriteAnimator') || {};
        const clip = sheet.resolveClip(animator, args.clip);
        const texturePath = sheet.textureForClip(sprite, clip);
        if (!texturePath) throw invalidArg('the sprite names no texture to slice', { id: args.id });

        const file = ctx.session.resolveAssetPath(texturePath);
        let doc;
        try {
            doc = imaging.loadImage(file);
        } catch (err) {
            throw invalidArg(`cannot read image: ${err.message}`, { file, texture: texturePath });
        }

        const rect = uvRectOf(sprite.uvRect);
        const grid = sheet.sheetGrid({
            width: doc.width,
            height: doc.height,
            columns: clip.columns,
            rows: clip.rows,
            rect,
        });
        const timeline = sheet.clipTimeline(grid, clip);
        const own = typeof animator.frame === 'number' && isFinite(animator.frame) ? animator.frame : 0;
        const wanted = args.frame === undefined ? own : args.frame;
        const frame = wanted < 0 ? 0 : Math.min(wanted, grid.count - 1);
        const clips = (Array.isArray(animator.clips) ? animator.clips : [])
            .map(c => (c && typeof c.name === 'string' ? c.name : ''))
            .filter(name => name !== '');

        let preview = null;
        if (args.output) {
            if (args.scale < 0 || args.scale > 64) throw invalidArg('--scale must be between 1 and 64');
            const scale = args.scale > 0
                ? args.scale
                : Math.max(1, Math.min(PREVIEW_MAX_SCALE, Math.round(PREVIEW_TARGET / Math.max(doc.width, doc.height))));
            preview = drawPreview(ctx.session.resolveOutputPath(args.output), doc, scale, grid, timeline,
                frame, args.grid);
        }

        return {
            id: args.id,
            name: entity.name || '',
            texture: ctx.session.toProjectRelative(file),
            textureSource: clip.texturePath !== '' ? 'animator' : 'sprite',
            width: doc.width,
            height: doc.height,
            columns: grid.columns,
            rows: grid.rows,
            rect: [rect.x, rect.y, rect.w, rect.h],
            region: grid.region,
            cell: grid.cell,
            count: grid.count,
            cells: grid.cells,
            clip: timeline,
            clips,
            frame,
            preview,
        };
    },
});
