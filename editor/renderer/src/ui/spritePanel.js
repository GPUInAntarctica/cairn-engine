// Cairn Engine editor - 2D panel.
//
// One panel for the sprite workflow: the sheet with its slicing drawn over it,
// the cells the animator's clip plays laid out as a strip weighted by their
// time, and a texel grid for the pixel work in between.
//
// The numbers come from editor/main/sheet.js, the same model the CLI's
// sprite.sheet reports, so the picture here and the numbers an agent reads
// cannot drift apart. The marks are the same too: cyan for the slicing,
// magenta for the cells the clip plays, a lit cell for the one about to be
// drawn - a screenshot of the preview PNG and this panel say the same thing.
'use strict';

import path from 'path';

import sheet from '../../../main/sheet.js';
import imaging from '../../../main/imaging.js';
import { editor, engineInstance, selectedEntity, markDirty } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';

const ZOOMS = [1, 2, 3, 4, 6, 8, 12, 16];
const MAX_FIT = 16;
const MARGIN = 24;          // room around a fitted sheet, in panel pixels
const PIXEL_GRID_ZOOM = 4;  // below this a line a texel is noise, not a grid
const MIN_CELL_W = 26;

const SLICE = '#00ffff';
const CLIP = '#ff00ff';
const LIT = 'rgba(255, 255, 255, 0.38)';
const TEXEL = 'rgba(255, 255, 255, 0.10)';

function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

function seconds(v) {
    const rounded = v >= 10 ? v.toFixed(1) : v.toFixed(2);
    return rounded + t('p2d.sec');
}

export class SpritePanel {
    constructor(pageEl) {
        this.pageEl = pageEl;
        this.zoom = 0;              // 0 = fit the sheet to the stage
        this.showCells = true;
        this.showPixelGrid = false;
        this.clipName = null;       // null follows the animator's own clip
        this.active = false;
        this.disposed = false;
        this.unsubscribes = [];
        this.hover = -1;
        this.state = null;
        this.image = null;          // {file, width, height, canvas} or {file, error}

        pageEl.classList.add('p2d-root');
        pageEl.innerHTML = `
            <div class="p2d-bar">
                <span class="p2d-title"></span>
                <span class="p2d-sub"></span>
                <span class="p2d-spacer"></span>
                <span class="p2d-group anim">
                    <button class="p2d-btn" data-act="prev" title="${escapeAttr(t('p2d.prevFrame'))}">&#9664;</button>
                    <button class="p2d-btn" data-act="play" title="${escapeAttr(t('p2d.play'))}"></button>
                    <button class="p2d-btn" data-act="next" title="${escapeAttr(t('p2d.nextFrame'))}">&#9654;</button>
                </span>
                <span class="p2d-group">
                    <button class="p2d-btn" data-act="zoomOut" title="${escapeAttr(t('p2d.zoomOut'))}">&minus;</button>
                    <span class="p2d-zoom"></span>
                    <button class="p2d-btn" data-act="zoomIn" title="${escapeAttr(t('p2d.zoomIn'))}">+</button>
                    <button class="p2d-btn" data-act="fit">${escapeHtml(t('p2d.fit'))}</button>
                    <button class="p2d-btn toggled" data-act="cells">${escapeHtml(t('p2d.cells'))}</button>
                    <button class="p2d-btn" data-act="pixel">${escapeHtml(t('p2d.pixelGrid'))}</button>
                </span>
            </div>
            <div class="p2d-stage">
                <canvas class="p2d-sheet"></canvas>
                <div class="p2d-hint"></div>
            </div>
            <div class="p2d-strip">
                <div class="p2d-strip-head">
                    <span class="p2d-label">${escapeHtml(t('p2d.clip'))}</span>
                    <select class="p2d-clip"></select>
                    <span class="p2d-len"></span>
                    <span class="p2d-spacer"></span>
                    <span class="p2d-read"></span>
                </div>
                <canvas class="p2d-track"></canvas>
            </div>`;

        this.titleEl = pageEl.querySelector('.p2d-title');
        this.subEl = pageEl.querySelector('.p2d-sub');
        this.zoomEl = pageEl.querySelector('.p2d-zoom');
        this.playEl = pageEl.querySelector('[data-act="play"]');
        this.animGroup = pageEl.querySelector('.anim');
        this.clipEl = pageEl.querySelector('.p2d-clip');
        this.lenEl = pageEl.querySelector('.p2d-len');
        this.readEl = pageEl.querySelector('.p2d-read');
        this.stageWrap = pageEl.querySelector('.p2d-stage');
        this.sheetCanvas = pageEl.querySelector('.p2d-sheet');
        this.hintEl = pageEl.querySelector('.p2d-hint');
        this.trackCanvas = pageEl.querySelector('.p2d-track');

        pageEl.addEventListener('click', ev => this.onClick(ev));
        this.stageWrap.addEventListener('mousemove', ev => this.onHover(ev));
        this.stageWrap.addEventListener('mouseleave', () => { this.hover = -1; this.readEl.textContent = ''; this.paintStage(); });
        this.clipEl.addEventListener('change', () => {
            // The picked clip becomes the animator's own, so a transition that
            // switches clips later is followed again.
            const picked = this.clipEl.value;
            this.clipName = null;
            this.setProp('clip', picked);
        });
        this.trackCanvas.addEventListener('mousemove', ev => this.onHover(ev, true));
        this.trackCanvas.addEventListener('click', ev => this.onTrackClick(ev));

        this.unsubscribes.push(
            on('selection-changed', () => this.paint()),
            on('scene-changed', () => this.paint()),
            on('play-state', () => this.paint()),
            on('entity-edited', () => this.paint()),
        );

        this.paint();
        this.tick();
    }

    // Closing the tab has to stop the playhead loop and drop the scene
    // listeners, or a closed panel keeps waking up for every edit.
    dispose() {
        this.disposed = true;
        for (const off of this.unsubscribes) off();
        this.unsubscribes = [];
    }

    // The tab system calls this so a playing clip only repaints while it is on
    // screen.
    setActive(active) {
        this.active = active;
        if (active) this.paint();
    }

    // The playhead follows the clip while it plays; everything else is event
    // driven, so this only has to look at the animator's own frame.
    tick() {
        requestAnimationFrame(() => this.tick());
        if (this.disposed || !this.active || !editor.playing || editor.paused) return;
        const st = this.state;
        if (!st || !st.animatorId) return;
        const frame = engineInstance().getComponentProp(st.animatorId, 'SpriteAnimator', 'frame');
        if (typeof frame === 'number' && frame !== st.frame) {
            st.frame = frame;
            this.paintStage();
            this.paintTrack();
        }
    }

    // ---------------------------------------------------------------- reading
    read() {
        const ent = selectedEntity();
        if (!ent) return null;
        const find = type => (ent.components || []).find(c => c.type === type);
        const sprite = find('SpriteRenderer');
        if (!sprite) return null;
        const animatorComp = find('SpriteAnimator');
        const animator = animatorComp ? (animatorComp.props || {}) : {};
        const clip = sheet.resolveClip(animator, this.clipName);
        const texture = sheet.textureForClip(sprite.props || {}, clip);
        const uv = (sprite.props && sprite.props.uvRect) || [];
        return {
            ent,
            sprite,
            animator,
            animatorId: animatorComp ? ent.id : 0,
            clip,
            texture,
            file: texture ? path.join(editor.project.path, texture) : '',
            uvRect: {
                x: typeof uv[0] === 'number' ? uv[0] : 0,
                y: typeof uv[1] === 'number' ? uv[1] : 0,
                w: typeof uv[2] === 'number' ? uv[2] : 1,
                h: typeof uv[3] === 'number' ? uv[3] : 1,
            },
            clips: (Array.isArray(animator.clips) ? animator.clips : [])
                .map(c => (c && typeof c.name === 'string' ? c.name : ''))
                .filter(name => name !== ''),
        };
    }

    // The sheet's pixels, decoded once per file. The image editor loads a PNG
    // the same way, so both panels show a texture the same way round.
    source(file) {
        if (!file) return null;
        if (this.image && this.image.file === file) return this.image;
        let doc = null;
        try {
            doc = imaging.loadImage(file);
        } catch {
            doc = null;
        }
        if (!doc) {
            this.image = { file, error: true };
            return this.image;
        }
        const canvas = document.createElement('canvas');
        canvas.width = doc.width;
        canvas.height = doc.height;
        const ctx = canvas.getContext('2d');
        const data = ctx.createImageData(doc.width, doc.height);
        data.data.set(imaging.composite(doc));
        ctx.putImageData(data, 0, 0);
        this.image = { file, width: doc.width, height: doc.height, canvas };
        return this.image;
    }

    // ------------------------------------------------------------------ paint
    paint() {
        const st = this.read();
        this.state = st;
        this.hover = -1;
        this.readEl.textContent = '';

        if (!st) {
            this.hintEl.textContent = t('p2d.noSprite');
            this.showHint(true);
            this.titleEl.textContent = t('panel.panel2d');
            this.subEl.textContent = '';
            this.clearTrack();
            return;
        }

        const img = this.source(st.file);
        st.image = img && !img.error ? img : null;
        if (!st.texture) {
            this.hintEl.textContent = t('p2d.noTexture');
        } else if (!st.image) {
            this.hintEl.textContent = t('p2d.missing', { file: st.texture });
        }
        this.showHint(!st.image);

        this.titleEl.textContent = st.ent.name;
        if (st.image) {
            st.grid = sheet.sheetGrid({
                width: st.image.width,
                height: st.image.height,
                columns: st.clip.columns,
                rows: st.clip.rows,
                rect: st.uvRect,
            });
            st.timeline = sheet.clipTimeline(st.grid, st.clip);
            const own = st.animator.frame;
            st.frame = clamp(typeof own === 'number' ? Math.trunc(own) : 0, 0, st.grid.count - 1);
            this.subEl.textContent = t('p2d.summary', {
                name: st.texture,
                size: `${st.image.width}\u00d7${st.image.height}`,
                cells: st.grid.count,
                grid: `${st.grid.columns}\u00d7${st.grid.rows}`,
            });
        } else {
            st.grid = null;
            st.timeline = null;
            st.frame = 0;
            this.subEl.textContent = st.texture || '';
        }

        this.paintControls(st);
        this.paintStage();
        this.paintTrack();
    }

    paintControls(st) {
        const hasAnimator = !!st.animatorId;
        this.animGroup.style.display = hasAnimator ? '' : 'none';
        this.clipEl.disabled = !hasAnimator;
        this.playEl.innerHTML = icon(st.animator.playing ? 'pause' : 'play', 12);
        this.playEl.title = st.animator.playing ? t('p2d.pause') : t('p2d.play');

        const names = st.clips.length ? st.clips : (st.clip.name ? [st.clip.name] : []);
        this.clipEl.innerHTML = names.length === 0
            ? `<option value="">${escapeHtml(t('p2d.noClip'))}</option>`
            : names.map(n => `<option value="${escapeAttr(n)}"${n === st.clip.name ? ' selected' : ''}>${escapeHtml(n)}</option>`).join('');
        this.clipEl.classList.toggle('off', !hasAnimator);

        const zoom = this.zoom > 0 ? `${this.zoom}\u00d7` : t('p2d.fit');
        this.zoomEl.textContent = zoom;
        this.lenEl.textContent = st.timeline
            ? t('p2d.length') + ' ' + seconds(st.timeline.length) + ' \u00b7 ' + t('p2d.frames', { count: st.timeline.count })
            : '';
    }

    showHint(on_) {
        this.hintEl.style.display = on_ ? '' : 'none';
    }

    zoomFor(img) {
        if (this.zoom > 0) return this.zoom;
        const W = this.stageWrap.clientWidth - MARGIN;
        const H = this.stageWrap.clientHeight - MARGIN;
        if (W <= 0 || H <= 0) return 1;
        return clamp(Math.floor(Math.min(W / img.width, H / img.height)), 1, MAX_FIT);
    }

    paintStage() {
        const st = this.state;
        const canvas = this.sheetCanvas;
        const W = this.stageWrap.clientWidth;
        const H = this.stageWrap.clientHeight;
        if (W <= 0 || H <= 0) return;
        canvas.width = W;
        canvas.height = H;
        const ctx = canvas.getContext('2d');
        ctx.imageSmoothingEnabled = false;
        ctx.clearRect(0, 0, W, H);
        if (!st || !st.image || !st.grid) return;

        const img = st.image;
        const zoom = this.zoomFor(img);
        const ox = Math.round((W - img.width * zoom) / 2);
        const oy = Math.round((H - img.height * zoom) / 2);
        ctx.drawImage(img.canvas, 0, 0, img.width, img.height, ox, oy, img.width * zoom, img.height * zoom);

        const mark = (x, y, w, h, color) => {
            ctx.fillStyle = color;
            ctx.fillRect(Math.round(x), Math.round(y), Math.max(1, Math.round(w)), Math.max(1, Math.round(h)));
        };
        const box = (x, y, w, h, color) => {
            mark(x, y, w, 1, color);
            mark(x, y + h - 1, w, 1, color);
            mark(x, y, 1, h, color);
            mark(x + w - 1, y, 1, h, color);
        };
        const at = (x, y) => [ox + x * zoom, oy + y * zoom];

        if (this.showPixelGrid && zoom >= PIXEL_GRID_ZOOM) {
            const lines = sheet.pixelGridLines({ width: img.width, height: img.height, zoom, stride: 1, originX: ox, originY: oy });
            for (const x of lines.xs) mark(x, oy, 1, img.height * zoom, TEXEL);
            for (const y of lines.ys) mark(ox, y, img.width * zoom, 1, TEXEL);
        }

        // The cell about to be drawn, under the slicing so its edges stay sharp.
        const lit = st.grid.cells[st.frame];
        if (lit && this.showCells) mark(...at(lit.x, lit.y), lit.w * zoom, lit.h * zoom, LIT);

        if (this.showCells) {
            for (const cell of st.grid.cells) {
                const [x, y] = at(cell.x, cell.y);
                box(x, y, cell.w * zoom, cell.h * zoom, SLICE);
            }
            for (const cell of st.timeline.cells) {
                const [x, y] = at(cell.x, cell.y);
                box(x, y, cell.w * zoom, cell.h * zoom, CLIP);
            }
            // A picked cell is marked where the eye is looking, over the rest.
            if (this.hover >= 0) {
                const cell = st.grid.cells[this.hover];
                const [x, y] = at(cell.x, cell.y);
                box(x, y, cell.w * zoom, cell.h * zoom, '#ffffff');
            }
        }

        // The pivot, in the cell that is playing: the point the sprite turns and
        // scales about, drawn where it lands in that cell's texels.
        const pivot = (st.sprite.props && st.sprite.props.pivot) || [0.5, 0.5];
        if (lit) {
            const px = ox + (lit.x + (pivot[0] || 0) * lit.w) * zoom;
            const py = oy + (lit.y + (pivot[1] || 0) * lit.h) * zoom;
            const accent = cssVar('--ac', '#6d9ee8');
            mark(px - 4, py, 9, 1, accent);
            mark(px, py - 4, 1, 9, accent);
        }
    }

    clearTrack() {
        const canvas = this.trackCanvas;
        const W = canvas.clientWidth;
        const H = canvas.clientHeight;
        canvas.width = Math.max(1, W);
        canvas.height = Math.max(1, H);
        canvas.getContext('2d').clearRect(0, 0, canvas.width, canvas.height);
    }

    paintTrack() {
        const canvas = this.trackCanvas;
        const W = canvas.clientWidth;
        const H = canvas.clientHeight;
        if (W <= 0 || H <= 0) return;
        canvas.width = W;
        canvas.height = H;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, W, H);
        const st = this.state;
        if (!st || !st.timeline) return;

        const cells = st.timeline.cells;
        const [widths, pad, gap] = trackLayout(cells, st.timeline.length, W);

        const top = 5;
        const boxH = Math.max(12, H - 10);
        let x = pad;
        for (let i = 0; i < cells.length; i++) {
            const cell = cells[i];
            const w = widths[i];
            const current = cell.frame === st.frame;
            ctx.fillStyle = current ? cssVar('--sel', '#2c4b72') : cssVar('--sf-raise', '#28282e');
            ctx.fillRect(x, top, w, boxH);
            ctx.fillStyle = current ? cssVar('--ac', '#6d9ee8') : cssVar('--bd-line', '#2b2b33');
            ctx.fillRect(x, top, w, 1);
            ctx.fillRect(x, top + boxH - 1, w, 1);
            ctx.fillRect(x, top, 1, boxH);
            ctx.fillRect(x + w - 1, top, 1, boxH);

            ctx.fillStyle = current ? cssVar('--tx-hi', '#e6e6ec') : cssVar('--tx-dim', '#8e8e99');
            ctx.font = `10px ${cssVar('--ui', 'sans-serif')}`;
            ctx.textAlign = 'left';
            ctx.textBaseline = 'top';
            if (w >= 18) ctx.fillText(String(cell.frame), x + 4, top + 3);
            if (w >= 42) {
                ctx.fillStyle = cssVar('--tx-faint', '#63636d');
                ctx.fillText(seconds(cell.seconds), x + 4, top + boxH - 13);
            }
            x += w + gap;
        }
    }

    // ----------------------------------------------------------- interaction
    cellAtEvent(ev, onTrack) {
        const st = this.state;
        if (!st || !st.image || !st.grid) return -1;
        if (onTrack) {
            const canvas = this.trackCanvas;
            const [widths, pad, gap] = trackLayout(st.timeline.cells, st.timeline.length, canvas.clientWidth);
            const rect = canvas.getBoundingClientRect();
            let x = pad;
            for (let i = 0; i < widths.length; i++) {
                if (ev.clientX - rect.left >= x && ev.clientX - rect.left < x + widths[i]) return st.timeline.cells[i].frame;
                x += widths[i] + gap;
            }
            return -1;
        }
        const rect = this.stageWrap.getBoundingClientRect();
        const img = st.image;
        const zoom = this.zoomFor(img);
        const ox = Math.round((this.stageWrap.clientWidth - img.width * zoom) / 2);
        const oy = Math.round((this.stageWrap.clientHeight - img.height * zoom) / 2);
        const tx = Math.floor((ev.clientX - rect.left - ox) / zoom);
        const ty = Math.floor((ev.clientY - rect.top - oy) / zoom);
        return sheet.cellAtPoint(st.grid, tx, ty);
    }

    onHover(ev, onTrack) {
        const st = this.state;
        if (!st || !st.grid) return;
        const cell = this.cellAtEvent(ev, onTrack);
        if (cell === this.hover) return;
        this.hover = cell;
        const c = cell >= 0 ? st.grid.cells[cell] : null;
        this.readEl.textContent = c ? t('p2d.cellAt', { frame: cell, w: c.w, h: c.h, x: c.x, y: c.y }) : '';
        if (!onTrack) this.paintStage();
        else this.paintTrack();
    }

    onTrackClick(ev) {
        const frame = this.cellAtEvent(ev, true);
        if (frame >= 0) this.setFrame(frame);
    }

    onClick(ev) {
        const button = ev.target.closest('.p2d-btn');
        if (!button) {
            if (ev.target === this.sheetCanvas || ev.target === this.stageWrap) {
                const frame = this.cellAtEvent(ev, false);
                if (frame >= 0) this.setFrame(frame);
            }
            return;
        }
        const st = this.state;
        const act = button.dataset.act;
        if (act === 'zoomIn' || act === 'zoomOut') {
            const img = st && st.image;
            const current = img ? this.zoomFor(img) : 1;
            const idx = ZOOMS.indexOf(current);
            const step = act === 'zoomIn' ? 1 : -1;
            this.zoom = idx < 0
                ? (step > 0 ? ZOOMS.find(z => z > current) || MAX_FIT : ZOOMS.filter(z => z < current).pop() || 1)
                : ZOOMS[clamp(idx + step, 0, ZOOMS.length - 1)];
            this.paintStage();
            this.paintControls(st);
            return;
        }
        if (act === 'fit') { this.zoom = 0; this.paintStage(); this.paintControls(st); return; }
        if (act === 'cells') {
            this.showCells = !this.showCells;
            button.classList.toggle('toggled', this.showCells);
            this.paintStage();
            return;
        }
        if (act === 'pixel') {
            this.showPixelGrid = !this.showPixelGrid;
            button.classList.toggle('toggled', this.showPixelGrid);
            this.paintStage();
            return;
        }
        if (!st || !st.animatorId) return;
        if (act === 'play') {
            this.setProp('playing', !st.animator.playing);
            return;
        }
        if (act === 'prev' || act === 'next') {
            const timeline = st.timeline;
            const delta = act === 'next' ? 1 : -1;
            let frame = st.frame + delta;
            if (frame > timeline.last) frame = timeline.first;
            if (frame < timeline.first) frame = timeline.last;
            this.setFrame(frame);
        }
    }

    setFrame(frame) {
        const st = this.state;
        if (!st || !st.animatorId) return;
        engineInstance().setComponentProp(st.animatorId, 'SpriteAnimator', 'frame', frame);
        markDirty();
        st.frame = frame;
        this.paintStage();
        this.paintTrack();
        emit('entity-edited');
    }

    setProp(name, value) {
        const st = this.state;
        if (!st || !st.animatorId) return;
        engineInstance().setComponentProp(st.animatorId, 'SpriteAnimator', name, value);
        markDirty();
        emit('entity-edited');
        this.paint();
    }
}

// The strip's boxes: one per cell, as wide as its share of the clip's time,
// and never narrower than a label. Both the drawing and the hit test go through
// here, so a click lands in the box that was drawn.
function trackLayout(cells, total, width) {
    const pad = 6;
    const gap = 2;
    const avail = Math.max(1, width - pad * 2 - gap * cells.length);
    let widths = cells.map(c => Math.max(MIN_CELL_W,
        Math.round((total > 0 ? c.seconds / total : 1 / cells.length) * avail)));
    const sum = widths.reduce((a, b) => a + b, 0);
    if (sum > avail) widths = widths.map(w => Math.max(8, Math.round(w * avail / sum)));
    return [widths, pad, gap];
}

function cssVar(name, fallback) {
    const value = getComputedStyle(document.documentElement).getPropertyValue(name);
    return value && value.trim() ? value.trim() : fallback;
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function escapeAttr(s) { return escapeHtml(s); }
