// Cairn Engine editor - sprite sheet model (main process).
//
// The editor's 2D panel and the CLI's sprite.sheet ask the same questions of the
// same data: which cells a sheet has, which of them a clip plays and for how
// long, which cell a point falls in, and which cell is showing after so many
// seconds. The answers live here so the panel and the command cannot disagree,
// and so they can be tested without a window.
//
// Everything mirrors SpriteAnimator. Cells are equal slices of the sprite's
// uvRect counted row-major from its top-left corner (CellRect). A clip is its
// firstFrame..lastFrame over the grid, with the component's own fields
// underneath a clip entry of the same name (ActiveClip). A cell's time is its
// `durations` entry when that is a positive number and the clip's 1 / fps
// otherwise (CellSeconds); a zero rate with no times gives zero-second cells,
// which is what the engine's stepper stalls on.
//
// The engine samples uvs as floats; the rects here are whole texels, laid edge
// to edge so a grid covers its region exactly. A cell can therefore differ from
// the engine's by one texel when a sheet's size is not a multiple of its grid -
// which only matters for the picture, never for which cell a frame names.
'use strict';

// ---------------------------------------------------------------- primitives
function num(v, def) {
    return typeof v === 'number' && isFinite(v) ? v : def;
}

function int(v, def) {
    return typeof v === 'number' && isFinite(v) ? Math.trunc(v) : def;
}

function bool(v, def) {
    return typeof v === 'boolean' ? v : def;
}

function str(v, def) {
    return typeof v === 'string' ? v : def;
}

// The engine's asFloat/asInt/asBool/asString take the value only when the JSON
// holds that type, so an entry's `"fps": "8"` keeps the component's 8 here too.

// ---------------------------------------------------------------- the clip
// The clip a component plays: the entry `name` names with the component's
// fields underneath it, or the component's fields alone when nothing matches.
// `name` defaults to the component's own `clip`.
function resolveClip(animator, name) {
    const a = animator || {};
    const out = {
        name: name === undefined || name === null ? str(a.clip, '') : String(name),
        columns: int(a.columns, 1),
        rows: int(a.rows, 1),
        texturePath: str(a.texturePath, ''),
        firstFrame: int(a.firstFrame, 0),
        lastFrame: int(a.lastFrame, -1),
        fps: num(a.fps, 8),
        loop: bool(a.loop, true),
        durations: Array.isArray(a.durations) ? a.durations : null,
    };
    const entries = Array.isArray(a.clips) ? a.clips : [];
    const entry = entries.find(c => c && c.name === out.name);
    if (!entry) return out;
    if (typeof entry.columns === 'number') out.columns = int(entry.columns, out.columns);
    if (typeof entry.rows === 'number') out.rows = int(entry.rows, out.rows);
    if (typeof entry.firstFrame === 'number') out.firstFrame = int(entry.firstFrame, out.firstFrame);
    if (typeof entry.lastFrame === 'number') out.lastFrame = int(entry.lastFrame, out.lastFrame);
    if (typeof entry.fps === 'number') out.fps = entry.fps;
    if (typeof entry.loop === 'boolean') out.loop = entry.loop;
    if (typeof entry.texturePath === 'string') out.texturePath = entry.texturePath;
    if (Array.isArray(entry.durations)) out.durations = entry.durations;
    return out;
}

// Which texture a clip draws: the one it names, the sprite's otherwise. The
// same rule the renderer follows, so the panel shows the sheet that is drawn.
// An empty path means the sprite's own texture, which the caller resolves.
function textureForClip(sprite, clip) {
    const own = clip && typeof clip.texturePath === 'string' ? clip.texturePath : '';
    if (own !== '') return own;
    return sprite && typeof sprite.texturePath === 'string' ? sprite.texturePath : '';
}

// ---------------------------------------------------------------- the grid
// Equal slices of the sprite's uvRect in whole texels, row-major from the
// region's top-left. Edges are rounded so the cells abut and cover the region
// exactly; the last cell of a row or column takes the remainder.
function sheetGrid(options) {
    const o = options || {};
    const width = Math.max(1, Math.round(num(o.width, 1)));
    const height = Math.max(1, Math.round(num(o.height, 1)));
    const columns = Math.max(1, int(o.columns, 1));
    const rows = Math.max(1, int(o.rows, 1));

    const r = o.rect || {};
    const rx = Math.round(num(r.x, 0) * width);
    const ry = Math.round(num(r.y, 0) * height);
    const rw = Math.round(num(r.w, 1) * width);
    const rh = Math.round(num(r.h, 1) * height);
    // A uvRect can sit outside the texture; the panel and the preview can only
    // show what is there, so the region is clamped to the sheet and kept at
    // least one texel wide.
    const x0 = Math.max(0, Math.min(width - 1, rx));
    const y0 = Math.max(0, Math.min(height - 1, ry));
    const x1 = Math.max(x0 + 1, Math.min(width, rx + rw));
    const y1 = Math.max(y0 + 1, Math.min(height, ry + rh));
    const region = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };

    const edge = (base, span, i, parts) => base + Math.round(span * i / parts);
    const cells = [];
    for (let row = 0; row < rows; row++) {
        const cy = edge(region.y, region.h, row, rows);
        const cy2 = edge(region.y, region.h, row + 1, rows);
        for (let column = 0; column < columns; column++) {
            const cx = edge(region.x, region.w, column, columns);
            const cx2 = edge(region.x, region.w, column + 1, columns);
            cells.push({
                index: row * columns + column,
                column,
                row,
                x: cx,
                y: cy,
                w: cx2 - cx,
                h: cy2 - cy,
            });
        }
    }
    return {
        width,
        height,
        columns,
        rows,
        region,
        cell: { width: Math.floor(region.w / columns), height: Math.floor(region.h / rows) },
        count: columns * rows,
        cells,
    };
}

// ---------------------------------------------------------------- the clip's cells
// One entry per cell the clip plays, in play order, with the time each one is
// shown and when it starts. `first`/`last`/`count` follow the engine's
// -1-means-the-end and out-of-range-clamps rules.
function clipTimeline(grid, clip) {
    const c = clip || {};
    const count0 = grid.count;
    const clampCell = v => (v < 0 ? 0 : Math.min(v, count0 - 1));
    const first = c.firstFrame < 0 ? 0 : clampCell(int(c.firstFrame, 0));
    const last = c.lastFrame < 0 ? count0 - 1 : clampCell(int(c.lastFrame, count0 - 1));
    const count = Math.max(1, last - first + 1);
    const fps = num(c.fps, 0);
    const step = fps > 0 ? 1 / fps : 0;
    const durations = Array.isArray(c.durations) ? c.durations : null;

    const cells = [];
    let t = 0;
    for (let i = 0; i < count; i++) {
        const frame = first + i;
        let seconds = step;
        if (durations && i < durations.length) {
            const d = durations[i];
            if (typeof d === 'number' && isFinite(d) && d > 0) seconds = d;
        }
        const g = grid.cells[frame];
        cells.push({
            index: i,
            frame,
            x: g.x,
            y: g.y,
            w: g.w,
            h: g.h,
            seconds,
            start: t,
            end: t + seconds,
        });
        t += seconds;
    }
    return {
        name: str(c.name, ''),
        first,
        last,
        count,
        fps,
        step,
        loop: bool(c.loop, true),
        cells,
        length: t,
    };
}

// Which cell is showing `seconds` into the clip, played from its first cell:
// time wraps around a looping clip and stops on the last cell of a one-shot
// one. A cell with no time of its own is one the engine's stepper stops on - it
// is reached at the time its start says and nothing past it ever plays - so the
// playhead sticks there instead of wrapping round it.
function frameAtPosition(timeline, seconds) {
    const count = timeline.count;
    const cells = timeline.cells;
    let t = num(seconds, 0);
    if (t < 0) t = 0;

    let stall = -1;
    for (let i = 0; i < count; i++) {
        if (!(cells[i].seconds > 0)) { stall = i; break; }
    }
    const reach = stall < 0 ? timeline.length : cells[stall].start;
    if (stall >= 0 && t >= reach) return { index: stall, frame: cells[stall].frame, local: 0, into: 0 };
    if (timeline.loop && reach > 0) t = ((t % reach) + reach) % reach;

    for (let i = 0; i < count; i++) {
        const cell = cells[i];
        if (!(cell.seconds > 0)) return { index: i, frame: cell.frame, local: 0, into: t };
        if (t < cell.seconds) return { index: i, frame: cell.frame, local: t / cell.seconds, into: t };
        t -= cell.seconds;
    }
    const last = cells[count - 1];
    return { index: count - 1, frame: last.frame, local: 1, into: last.seconds };
}

// The cell a texel falls in, or -1 outside the grid's region. Coordinates are
// in the sheet's own texels, counted from its top-left corner.
function cellAtPoint(grid, x, y) {
    if (!(x >= grid.region.x && x < grid.region.x + grid.region.w)) return -1;
    if (!(y >= grid.region.y && y < grid.region.y + grid.region.h)) return -1;
    for (const cell of grid.cells) {
        if (x >= cell.x && x < cell.x + cell.w && y >= cell.y && y < cell.y + cell.h) return cell.index;
    }
    return -1;
}

// The panel's texel grid: one line every `stride` texels, at the boundary
// between two of them, in the panel's own pixels (`origin` + texel * zoom).
// Lines on the sheet's outer edge are left out - the panel draws the sheet's
// border itself, which does not move with the scroll position.
function pixelGridLines(options) {
    const o = options || {};
    const width = Math.max(0, Math.round(num(o.width, 0)));
    const height = Math.max(0, Math.round(num(o.height, 0)));
    const zoom = num(o.zoom, 1);
    const stride = Math.max(1, int(o.stride, 1));
    const originX = num(o.originX, 0);
    const originY = num(o.originY, 0);
    const xs = [];
    const ys = [];
    for (let x = stride; x < width; x += stride) xs.push(originX + x * zoom);
    for (let y = stride; y < height; y += stride) ys.push(originY + y * zoom);
    return { stride, xs, ys };
}

module.exports = {
    resolveClip,
    textureForClip,
    sheetGrid,
    clipTimeline,
    frameAtPosition,
    cellAtPoint,
    pixelGridLines,
};
