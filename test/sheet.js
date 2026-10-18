#!/usr/bin/env node
// Cairn Engine - sprite sheet model test.
//
// editor/main/sheet.js turns an animator's numbers into the cells a sheet has,
// the cells a clip plays, and which of them is showing after so many seconds.
// The editor's 2D panel draws that model and the CLI's sprite.sheet prints it,
// so the model is where a slicing or timing rule is decided. These checks pin
// the rules the engine's SpriteAnimator follows: the uvRect region, the
// row-major cell order, -1 meaning the end of the grid, out-of-range frames
// clamping, the per-cell times and the wrap of a looping clip.
'use strict';

const path = require('path');

const root = path.resolve(__dirname, '..');
const sheet = require(path.join(root, 'editor', 'main', 'sheet.js'));

let failures = 0;
let checks = 0;
function check(cond, label, extra) {
    checks++;
    if (cond) { console.log(`  ok  ${label}`); }
    else { console.error(`FAIL  ${label}${extra ? '\n      ' + extra : ''}`); failures++; }
}
function eq(a, b, label) {
    check(a === b, label, `expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`);
}

// ------------------------------------------------------------------- the grid
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    eq(g.count, 4, 'a 2x2 grid over a 16x16 sheet has four cells');
    eq(JSON.stringify(g.region), JSON.stringify({ x: 0, y: 0, w: 16, h: 16 }), 'no uvRect means the whole sheet');
    eq(JSON.stringify(g.cells.map(c => [c.x, c.y, c.w, c.h])),
        JSON.stringify([[0, 0, 8, 8], [8, 0, 8, 8], [0, 8, 8, 8], [8, 8, 8, 8]]),
        'cells run row-major from the top-left at 8x8');
    eq(JSON.stringify(g.cell), JSON.stringify({ width: 8, height: 8 }), 'the nominal cell size is the region over the grid');
}
{
    // A uvRect that names the bottom half: the cells move with it.
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 1, rows: 2, rect: { x: 0, y: 0.5, w: 1, h: 0.5 } });
    eq(JSON.stringify(g.region), JSON.stringify({ x: 0, y: 8, w: 16, h: 8 }), 'a uvRect becomes a texel region');
    eq(JSON.stringify(g.cells.map(c => [c.x, c.y, c.w, c.h])),
        JSON.stringify([[0, 8, 16, 4], [0, 12, 16, 4]]), 'the grid is laid inside the region');
}
{
    // 10 does not divide by 3: the last cell of each line takes the remainder,
    // so the cells abut and cover the region exactly.
    const g = sheet.sheetGrid({ width: 10, height: 10, columns: 3, rows: 3 });
    eq(JSON.stringify(g.cells.slice(0, 3).map(c => [c.x, c.w])),
        JSON.stringify([[0, 3], [3, 4], [7, 3]]), 'a remainder goes to the last column');
    eq(JSON.stringify(g.cells.slice(6, 9).map(c => [c.y, c.h])),
        JSON.stringify([[7, 3], [7, 3], [7, 3]]), 'rows take their remainder the same way');
    const covered = g.cells.every(c => c.x >= g.region.x && c.x + c.w <= g.region.x + g.region.w &&
        c.y >= g.region.y && c.y + c.h <= g.region.y + g.region.h);
    check(covered, 'every cell stays inside the region');
    eq(g.cells[0].x + g.cells[0].w, g.cells[1].x, 'neighbouring cells share an edge');
    eq(g.cells[2].x + g.cells[2].w, 10, 'the last column ends at the region edge');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 0, rows: -5 });
    eq(g.count, 1, 'a grid of zero or fewer lines is one cell');
    eq(g.cells[0].w, 16, 'and that cell is the whole region');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2, rect: { x: -0.5, y: 0, w: 2, h: 2 } });
    eq(JSON.stringify(g.region), JSON.stringify({ x: 0, y: 0, w: 16, h: 16 }), 'a region outside the sheet is clamped to it');
}
{
    // A sheet smaller than its grid still gives every cell at least one texel.
    const g = sheet.sheetGrid({ width: 2, height: 2, columns: 4, rows: 1 });
    const widths = g.cells.map(c => c.w).filter(w => w > 0);
    check(widths.length > 0 && widths.every(w => w >= 1), 'cells keep at least one texel');
    eq(g.cells[0].x, 0, 'the first cell starts at the region edge');
}

// ------------------------------------------------------------------- a clip
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    const t = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 4 });
    eq(t.count, 4, '-1 as the last frame means the last cell of the grid');
    eq(JSON.stringify(t.cells.map(c => c.frame)), JSON.stringify([0, 1, 2, 3]), 'the clip plays the whole grid in order');
    eq(JSON.stringify(t.cells.map(c => c.seconds)), JSON.stringify([0.25, 0.25, 0.25, 0.25]), 'a rate gives every cell 1 / fps');
    eq(JSON.stringify(t.cells.map(c => c.start)), JSON.stringify([0, 0.25, 0.5, 0.75]), 'each cell starts where the last ended');
    eq(t.length, 1, 'the clip lasts as long as its cells');
    eq(JSON.stringify(t.cells[2]), JSON.stringify(t.cells[2]), 'cells carry their own rect');
    eq(t.cells[2].x + ',' + t.cells[2].y, '0,8', 'a clip cell keeps the grid rect of its frame');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 4, rows: 1 });
    const t = sheet.clipTimeline(g, { firstFrame: 1, lastFrame: 2, fps: 8, durations: [0.5, 'x', 0.25, 0.125] });
    eq(t.count, 2, 'firstFrame and lastFrame pick a run of cells');
    eq(JSON.stringify(t.cells.map(c => [c.index, c.frame])), JSON.stringify([[0, 1], [1, 2]]),
        'a cell knows its place in the clip and in the grid');
    eq(JSON.stringify(t.cells.map(c => c.seconds)), JSON.stringify([0.5, 0.125]),
        'a cell time is counted from the clip\'s first cell, not the grid\'s');
    eq(t.length, 0.625, 'the length adds the cell times up');
    eq(t.step, 0.125, 'the rate is still reported');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 4, rows: 1 });
    const t = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 4, durations: [0, -1, 2] });
    eq(JSON.stringify(t.cells.map(c => c.seconds)), JSON.stringify([0.25, 0.25, 2, 0.25]),
        'a cell time that is not a positive number falls back to the rate');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    const t = sheet.clipTimeline(g, { firstFrame: 3, lastFrame: 20, fps: 4 });
    eq(t.first + ',' + t.last + ',' + t.count, '3,3,1', 'out-of-range frames clamp into the grid');
    const past = sheet.clipTimeline(g, { firstFrame: 9, lastFrame: -1, fps: 4 });
    eq(past.first + ',' + past.last + ',' + past.count, '3,3,1', 'a first frame past the end clamps to the last cell');
    const behind = sheet.clipTimeline(g, { firstFrame: -4, lastFrame: 1, fps: 4 });
    eq(behind.first, 0, 'and one before the start clamps to the first');
    const odd = sheet.clipTimeline(g, { firstFrame: 2, lastFrame: 1, fps: 4 });
    eq(odd.count, 1, 'a clip whose first frame is past its last still plays one cell');
    eq(odd.cells[0].frame, 2, 'and that cell is the first frame');
    const none = sheet.clipTimeline(g, { firstFrame: -1, lastFrame: -1, fps: 4 });
    eq(none.count, 4, '-1 at both ends means the whole grid');
    const stalled = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 0 });
    eq(JSON.stringify(stalled.cells.map(c => c.seconds)), JSON.stringify([0, 0, 0, 0]),
        'no rate and no times leaves every cell without time');
}

// ------------------------------------------------------------------ the playhead
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    const t = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 4, loop: true });
    eq(sheet.frameAtPosition(t, 0).index, 0, 'play starts on the first cell');
    eq(sheet.frameAtPosition(t, 0.24).index, 0, 'a cell is shown until its time is up');
    eq(sheet.frameAtPosition(t, 0.26).index, 1, 'and then the next one takes over');
    eq(sheet.frameAtPosition(t, 0.125).local, 0.5, 'the playhead knows how far into the cell it is');
    eq(sheet.frameAtPosition(t, 1.01).frame, 0, 'a looping clip wraps round to its first cell');
    eq(sheet.frameAtPosition(t, 2.5).index, 2, 'and keeps wrapping');
    eq(sheet.frameAtPosition(t, -1).index, 0, 'time before the start is the start');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    const t = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 4, loop: false });
    eq(sheet.frameAtPosition(t, 0.9).index, 3, 'a clip that does not loop runs to its last cell');
    eq(sheet.frameAtPosition(t, 5).index, 3, 'and stays there');
    eq(sheet.frameAtPosition(t, 5).local, 1, 'at the very end of the last cell');
}
{
    // A cell with no time of its own is where the engine's stepper stops.
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 4, rows: 1 });
    const t = sheet.clipTimeline(g, { firstFrame: 0, lastFrame: -1, fps: 0, loop: true, durations: [0.5] });
    eq(sheet.frameAtPosition(t, 0.25).index, 0, 'a cell with a time of its own is shown');
    eq(sheet.frameAtPosition(t, 0.5).index, 1, 'a cell with no time of its own is reached when its start does');
    eq(sheet.frameAtPosition(t, 1.5).index, 1, 'a cell with no time of its own stops the walk');
    eq(sheet.frameAtPosition(t, 1.5).into, 0, 'and the playhead sits at its start');
}

// ------------------------------------------------------------------ the point
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 2, rows: 2 });
    eq(sheet.cellAtPoint(g, 0, 0), 0, 'the top-left texel is the first cell');
    eq(sheet.cellAtPoint(g, 7, 7), 0, 'a point stays in its cell up to its last texel');
    eq(sheet.cellAtPoint(g, 8, 0), 1, 'the next column is the next cell');
    eq(sheet.cellAtPoint(g, 0, 8), 2, 'the next row is a grid width further on');
    eq(sheet.cellAtPoint(g, 15, 15), 3, 'the bottom-right texel is the last cell');
    eq(sheet.cellAtPoint(g, 16, 0), -1, 'a point past the right edge is in no cell');
    eq(sheet.cellAtPoint(g, -1, 4), -1, 'and neither is one before the left edge');
}
{
    const g = sheet.sheetGrid({ width: 16, height: 16, columns: 1, rows: 2, rect: { x: 0, y: 0.5, w: 1, h: 0.5 } });
    eq(sheet.cellAtPoint(g, 4, 4), -1, 'a point above the region is in no cell');
    eq(sheet.cellAtPoint(g, 4, 8), 0, 'the region decides where the cells are');
    eq(sheet.cellAtPoint(g, 4, 15), 1, 'and where its second row is');
}
{
    const g = sheet.sheetGrid({ width: 10, height: 10, columns: 3, rows: 1 });
    eq(JSON.stringify([0, 1, 2, 3, 6, 7, 9].map(x => sheet.cellAtPoint(g, x, 0))),
        JSON.stringify([0, 0, 0, 1, 1, 2, 2]), 'a point falls in the cell whose texels cover it');
}

// ------------------------------------------------------------------ the pixel grid
{
    const grid = sheet.pixelGridLines({ width: 4, height: 3, zoom: 4 });
    eq(JSON.stringify(grid.xs), JSON.stringify([4, 8, 12]), 'a line sits between texels, not on them');
    eq(JSON.stringify(grid.ys), JSON.stringify([4, 8]), 'rows get a line each as well');
    eq(grid.stride, 1, 'the default stride is one texel');
}
{
    const grid = sheet.pixelGridLines({ width: 4, height: 4, zoom: 4, stride: 2, originX: 10, originY: 20 });
    eq(JSON.stringify(grid.xs), JSON.stringify([18]), 'a stride skips lines');
    eq(JSON.stringify(grid.ys), JSON.stringify([28]), 'in both directions');
}
{
    const grid = sheet.pixelGridLines({ width: 2, height: 2, zoom: 1 });
    eq(grid.xs.length + grid.ys.length, 2, 'a sheet with no interior boundary gets no lines');
}

// -------------------------------------------------------------- the clip itself
{
    const c = sheet.resolveClip({ clip: 'walk', columns: 2, rows: 2, fps: 8, loop: true, firstFrame: 0, lastFrame: -1 });
    eq(c.name, 'walk', 'a clip is named after the component\'s own clip field');
    eq(c.fps, 8, 'and starts from the component\'s fields');
    const entry = sheet.resolveClip({
        clip: 'walk', columns: 1, rows: 1, fps: 8, firstFrame: 0, lastFrame: -1, durations: [1],
        clips: [{ name: 'walk', columns: 4, fps: '9', loop: 'no', durations: [], firstFrame: 4 }],
    });
    eq(entry.columns + ',' + entry.firstFrame, '4,4', 'an entry overrides the fields it names');
    eq(entry.fps + ',' + entry.loop, '8,true', 'a field of the wrong type is not an override');
    eq(JSON.stringify(entry.durations), '[]', 'an empty durations array clears the component\'s');
    const other = sheet.resolveClip({ clip: 'walk', columns: 2, clips: [{ name: 'idle', columns: 4 }] }, 'idle');
    eq(other.columns + ',' + other.name, '4,idle', 'a named clip is looked up by name');
    const missing = sheet.resolveClip({ clip: 'walk', columns: 2, clips: [{ name: 'idle', columns: 4 }] }, 'jump');
    eq(missing.columns + ',' + missing.name, '2,jump', 'a name with no entry keeps the component\'s fields');
    const odd = sheet.resolveClip({ clip: 'walk', durations: [1, 2], clips: [{ name: 'walk', durations: 'nope' }] });
    eq(JSON.stringify(odd.durations), JSON.stringify([1, 2]), 'an entry can only replace times with an array');
    const none = sheet.resolveClip(null);
    eq(none.columns + ',' + none.rows + ',' + none.fps + ',' + none.loop + ',' + none.lastFrame, '1,1,8,true,-1',
        'no animator at all still describes one whole-sheet cell at the default rate');
}
{
    eq(sheet.textureForClip({ texturePath: 'hero.png' }, { texturePath: '' }), 'hero.png',
        'a clip that names no texture draws the sprite\'s');
    eq(sheet.textureForClip({ texturePath: 'hero.png' }, { texturePath: 'walk.png' }), 'walk.png',
        'a clip that names one draws its own');
    eq(sheet.textureForClip(null, null), '', 'and with neither there is no texture to name');
}

console.log(`sheet: ${checks - failures}/${checks} checks passed`);
process.exit(failures === 0 ? 0 : 1);
