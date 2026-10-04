// Cairn Engine editor - image editor.
// Layered raster painting with brush/eraser/fill/shape tools, HSV picker,
// undo history and a height-to-normal-map generator for PBR texturing.
'use strict';

import fs from 'fs';
import path from 'path';

import { editor } from './state.js';
import { t } from './i18n.js';
import { icon } from './icons.js';
import { ipc } from './ipc.js';

const MAX_HISTORY = 40;
const SWATCHES = ['#1b1b21', '#ffffff', '#d05a5a', '#d8a04a', '#e8d75a', '#6fbf73', '#5a8fd0', '#8a6fd0', '#d06fa8', '#8b8b97'];

export class ImageEditor {
    // filePath may be '' for an untitled document.
    constructor(pageEl, filePath) {
        this.pageEl = pageEl;
        this.filePath = filePath || '';
        this.zoom = 1;
        this.tool = 'brush';
        this.brushSize = 8;
        this.opacity = 1;
        this.hardness = 0.8;
        this.color = '#e8e8ee';
        this.showGrid = false;
        this.pixelSampling = false;
        this.history = [];
        this.historyIndex = -1;
        this.spaceDown = false;

        this.buildUi();
        if (filePath && fs.existsSync(filePath)) {
            this.loadFile(filePath);
        } else {
            this.newDocument(256, 256, true);
        }
    }

    // ------------------------------------------------------------------ UI
    buildUi() {
        const el = this.pageEl;
        el.innerHTML = `
            <div style="display:flex; align-items:center; gap:4px; padding:3px 8px; background:var(--panel-2); border-bottom:1px solid var(--border); flex:none;">
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="new">${t('imged.newImage')}</button>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="open">${t('imged.open')}</button>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="save">${t('imged.save')}</button>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="saveas">${t('imged.saveAs')}</button>
                <span style="width:1px; height:14px; background:var(--border-hi); margin:0 4px;"></span>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="undo">${t('imged.undo')}</button>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="redo">${t('imged.redo')}</button>
                <span style="width:1px; height:14px; background:var(--border-hi); margin:0 4px;"></span>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="normal">${t('imged.normalMap')}</button>
                <span style="flex:1;"></span>
                <span class="muted" id="imged-pos" style="font:11px var(--mono);"></span>
                <span class="muted" id="imged-dim" style="font:11px var(--mono);"></span>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="grid">${t('imged.grid')}</button>
                <button class="btn" style="height:22px; padding:0 8px; font-size:11px;" data-act="fit">${t('imged.fit')}</button>
            </div>
            <div class="imged-root">
                <div class="imged-tools"></div>
                <div class="imged-canvas-wrap">
                    <div class="imged-canvas-stack" id="imged-stack">
                        <canvas class="base" id="imged-composite"></canvas>
                    </div>
                </div>
                <div class="imged-side">
                    <div class="section">
                        <h5>${t('imged.brush')}</h5>
                        <div class="prop-row" style="grid-template-columns:70px 1fr;">
                            <label class="no-drag" style="cursor:default;">${t('imged.size')}</label>
                            <input type="range" id="ie-size" min="1" max="128" value="8">
                        </div>
                        <div class="prop-row" style="grid-template-columns:70px 1fr;">
                            <label class="no-drag" style="cursor:default;">${t('imged.opacity')}</label>
                            <input type="range" id="ie-opacity" min="1" max="100" value="100">
                        </div>
                        <div class="prop-row" style="grid-template-columns:70px 1fr;">
                            <label class="no-drag" style="cursor:default;">${t('imged.hardness')}</label>
                            <input type="range" id="ie-hardness" min="0" max="100" value="80">
                        </div>
                    </div>
                    <div class="section">
                        <h5>${t('imged.color')}</h5>
                        <div class="sv-picker" id="ie-sv"></div>
                        <div class="hue-strip" id="ie-hue"></div>
                        <div style="display:flex; gap:6px; align-items:center;">
                            <input type="color" id="ie-color" value="#e8e8ee" style="width:34px;">
                            <input type="text" id="ie-hex" value="#E8E8EE" style="flex:1; font:11px var(--mono);" spellcheck="false">
                        </div>
                        <div class="swatch-row" id="ie-swatches"></div>
                    </div>
                    <div class="section" style="flex-direction:row; gap:6px; align-items:center;">
                        <h5 style="margin-right:auto;">${t('imged.layers')}</h5>
                        <button class="btn" style="height:20px; padding:0 6px; font-size:11px;" data-act="layer-add">+</button>
                        <button class="btn" style="height:20px; padding:0 6px; font-size:11px;" data-act="layer-dup">${t('imged.layer.dup')}</button>
                        <button class="btn" style="height:20px; padding:0 6px; font-size:11px;" data-act="layer-del">${t('imged.layer.del')}</button>
                        <button class="btn" style="height:20px; padding:0 6px; font-size:11px;" data-act="layer-merge">↓</button>
                    </div>
                    <div class="imged-layers" id="ie-layers"></div>
                </div>
            </div>`;

        this.toolsEl = el.querySelector('.imged-tools');
        this.wrap = el.querySelector('.imged-canvas-wrap');
        this.stack = el.querySelector('#imged-stack');
        this.composite = el.querySelector('#imged-composite');
        this.compCtx = this.composite.getContext('2d');
        this.posEl = el.querySelector('#imged-pos');
        this.dimEl = el.querySelector('#imged-dim');

        const toolDefs = [
            ['brush', 'imged.brush'], ['eraser', 'imged.eraser'], ['fill', 'imged.fill'],
            ['line', 'imged.line'], ['rect', 'imged.rect'], ['ellipse', 'imged.ellipse'],
            ['picker', 'imged.picker'], ['pan', 'imged.pan'],
        ];
        for (const [id, label] of toolDefs) {
            const b = document.createElement('div');
            b.className = 'imged-tool' + (id === this.tool ? ' active' : '');
            b.dataset.tool = id;
            b.title = t(label);
            b.innerHTML = icon(id === 'picker' ? 'picker' : id, 15);
            b.addEventListener('click', () => this.setTool(id));
            this.toolsEl.appendChild(b);
        }

        // Sliders.
        el.querySelector('#ie-size').addEventListener('input', ev => { this.brushSize = +ev.target.value; });
        el.querySelector('#ie-opacity').addEventListener('input', ev => { this.opacity = +ev.target.value / 100; });
        el.querySelector('#ie-hardness').addEventListener('input', ev => { this.hardness = +ev.target.value / 100; });

        // Color picker.
        this.svEl = el.querySelector('#ie-sv');
        this.hueEl = el.querySelector('#ie-hue');
        this.colorInput = el.querySelector('#ie-color');
        this.hexInput = el.querySelector('#ie-hex');
        this.hsv = { h: 0, s: 0, v: 0.93 };
        this.renderPicker();
        this.colorInput.addEventListener('input', () => this.setColor(this.colorInput.value, true));
        this.hexInput.addEventListener('change', () => this.setColor(this.hexInput.value.trim(), true));

        const sw = el.querySelector('#ie-swatches');
        for (const c of SWATCHES) {
            const s = document.createElement('div');
            s.className = 'swatch';
            s.style.background = c;
            s.title = c;
            s.addEventListener('click', () => this.setColor(c, true));
            sw.appendChild(s);
        }

        // Actions.
        el.querySelector('[data-act=new]').addEventListener('click', () => this.newImageDialog());
        el.querySelector('[data-act=open]').addEventListener('click', () => this.openDialog());
        el.querySelector('[data-act=save]').addEventListener('click', () => this.save(false));
        el.querySelector('[data-act=saveas]').addEventListener('click', () => this.save(true));
        el.querySelector('[data-act=undo]').addEventListener('click', () => this.undo());
        el.querySelector('[data-act=redo]').addEventListener('click', () => this.redo());
        el.querySelector('[data-act=normal]').addEventListener('click', () => this.normalMapDialog());
        el.querySelector('[data-act=grid]').addEventListener('click', () => {
            this.showGrid = !this.showGrid;
            this.render();
        });
        el.querySelector('[data-act=fit]').addEventListener('click', () => this.fitZoom());

        el.querySelector('[data-act=layer-add]').addEventListener('click', () => {
            this.pushHistory();
            const c = document.createElement('canvas');
            c.width = this.doc.width; c.height = this.doc.height;
            this.doc.layers.splice(this.activeLayer + 1, 0,
                { name: `Layer ${this.doc.layers.length + 1}`, canvas: c, visible: true, opacity: 1 });
            this.activeLayer++;
            this.renderLayers();
            this.render();
        });
        el.querySelector('[data-act=layer-dup]').addEventListener('click', () => {
            this.pushHistory();
            const src = this.doc.layers[this.activeLayer];
            const c = document.createElement('canvas');
            c.width = this.doc.width; c.height = this.doc.height;
            c.getContext('2d').drawImage(src.canvas, 0, 0);
            this.doc.layers.splice(this.activeLayer + 1, 0,
                { name: src.name + ' copy', canvas: c, visible: src.visible, opacity: src.opacity });
            this.activeLayer++;
            this.renderLayers();
            this.render();
        });
        el.querySelector('[data-act=layer-del]').addEventListener('click', () => {
            if (this.doc.layers.length <= 1) return;
            this.pushHistory();
            this.doc.layers.splice(this.activeLayer, 1);
            this.activeLayer = Math.min(this.activeLayer, this.doc.layers.length - 1);
            this.renderLayers();
            this.render();
        });
        el.querySelector('[data-act=layer-merge]').addEventListener('click', () => {
            if (this.activeLayer === 0) return;
            this.pushHistory();
            const dst = this.doc.layers[this.activeLayer - 1];
            const src = this.doc.layers[this.activeLayer];
            const ctx = dst.canvas.getContext('2d');
            ctx.globalAlpha = src.opacity;
            ctx.drawImage(src.canvas, 0, 0);
            ctx.globalAlpha = 1;
            this.doc.layers.splice(this.activeLayer, 1);
            this.activeLayer--;
            this.renderLayers();
            this.render();
        });

        this.bindCanvasEvents();
        window.addEventListener('keydown', ev => {
            if (ev.code === 'Space' && this.wrap.matches(':hover')) { this.spaceDown = true; ev.preventDefault(); }
        });
        window.addEventListener('keyup', ev => { if (ev.code === 'Space') this.spaceDown = false; });
        this.wrap.addEventListener('wheel', ev => {
            ev.preventDefault();
            const factor = ev.deltaY < 0 ? 1.15 : 1 / 1.15;
            this.zoom = Math.min(16, Math.max(0.1, this.zoom * factor));
            this.render();
        }, { passive: false });
    }

    setTool(id) {
        this.tool = id;
        this.toolsEl.querySelectorAll('.imged-tool').forEach(b =>
            b.classList.toggle('active', b.dataset.tool === id));
    }

    // ------------------------------------------------------------ documents
    newDocument(width, height, withBase) {
        this.doc = { width, height, layers: [] };
        this.activeLayer = 0;
        this.history = [];
        this.historyIndex = -1;
        if (withBase) {
            const c = document.createElement('canvas');
            c.width = width; c.height = height;
            const ctx = c.getContext('2d');
            ctx.fillStyle = '#ffffff';
            ctx.fillRect(0, 0, width, height);
            this.doc.layers.push({ name: 'Background', canvas: c, visible: true, opacity: 1 });
        }
        this.zoomFit();
        this.renderLayers();
        this.render();
    }

    loadFile(filePath) {
        const img = new Image();
        img.onload = () => {
            const c = document.createElement('canvas');
            c.width = img.naturalWidth; c.height = img.naturalHeight;
            c.getContext('2d').drawImage(img, 0, 0);
            this.doc = { width: c.width, height: c.height, layers: [
                { name: path.basename(filePath), canvas: c, visible: true, opacity: 1 },
            ] };
            this.activeLayer = 0;
            this.history = [];
            this.historyIndex = -1;
            this.zoomFit();
            this.renderLayers();
            this.render();
        };
        img.src = 'file://' + filePath.replace(/\\/g, '/');
    }

    zoomFit() {
        if (!this.doc) return;
        const rect = this.wrap.getBoundingClientRect();
        this.zoom = Math.min(4, Math.max(0.05, Math.min(
            (rect.width - 60) / this.doc.width, (rect.height - 60) / this.doc.height)));
    }

    // -------------------------------------------------------------- history
    pushHistory() {
        // Snapshot only the affected layer plus structural info.
        const snapshot = {
            activeLayer: this.activeLayer,
            doc: this.doc.layers.map(l => ({
                name: l.name, visible: l.visible, opacity: l.opacity,
                data: l.canvas.toDataURL('image/png'),
            })),
        };
        this.history = this.history.slice(0, this.historyIndex + 1);
        this.history.push(snapshot);
        if (this.history.length > MAX_HISTORY) this.history.shift();
        this.historyIndex = this.history.length - 1;
    }

    restore(entry) {
        this.doc.layers = entry.doc.map(l => {
            const c = document.createElement('canvas');
            c.width = this.doc.width; c.height = this.doc.height;
            const img = new Image();
            // Synchronous-ish decode via data URL is async; draw when ready.
            img.onload = () => { c.getContext('2d').drawImage(img, 0, 0); this.render(); };
            img.src = l.data;
            return { name: l.name, canvas: c, visible: l.visible, opacity: l.opacity };
        });
        this.activeLayer = Math.min(entry.activeLayer, this.doc.layers.length - 1);
        this.renderLayers();
        this.render();
    }

    undo() {
        if (this.historyIndex < 0) return;
        if (this.historyIndex === this.history.length - 1) this.pushHistory(); // current state
        this.historyIndex--;
        this.restore(this.history[this.historyIndex]);
    }

    redo() {
        if (this.historyIndex >= this.history.length - 1) return;
        this.historyIndex++;
        this.restore(this.history[this.historyIndex]);
    }

    // --------------------------------------------------------------- render
    render() {
        if (!this.doc) return;
        const w = Math.max(1, Math.round(this.doc.width * this.zoom));
        const h = Math.max(1, Math.round(this.doc.height * this.zoom));
        this.composite.width = w;
        this.composite.height = h;
        this.composite.style.width = w + 'px';
        this.composite.style.height = h + 'px';
        this.compCtx.imageSmoothingEnabled = !this.pixelSampling;
        this.compCtx.clearRect(0, 0, w, h);
        for (const l of this.doc.layers) {
            if (!l.visible) continue;
            this.compCtx.globalAlpha = l.opacity;
            this.compCtx.drawImage(l.canvas, 0, 0, w, h);
        }
        this.compCtx.globalAlpha = 1;
        this.dimEl.textContent = `${this.doc.width}×${this.doc.height} · ${Math.round(this.zoom * 100)}%`;

        // Rubber-band preview canvas.
        if (this.preview && this.previewCanvas) {
            this.compCtx.drawImage(this.previewCanvas, 0, 0, w, h);
        }
        if (this.showGrid && this.zoom >= 4) {
            this.compCtx.strokeStyle = 'rgba(128,128,160,0.25)';
            this.compCtx.lineWidth = 1;
            this.compCtx.beginPath();
            for (let x = 0; x <= this.doc.width; ++x) {
                this.compCtx.moveTo(Math.round(x * this.zoom), 0);
                this.compCtx.lineTo(Math.round(x * this.zoom), h);
            }
            for (let y = 0; y <= this.doc.height; ++y) {
                this.compCtx.moveTo(0, Math.round(y * this.zoom));
                this.compCtx.lineTo(w, Math.round(y * this.zoom));
            }
            this.compCtx.stroke();
        }
    }

    renderLayers() {
        const host = this.pageEl.querySelector('#ie-layers');
        if (!host) return;
        host.innerHTML = '';
        for (let i = this.doc.layers.length - 1; i >= 0; --i) {
            const l = this.doc.layers[i];
            const row = document.createElement('div');
            row.className = 'layer-row' + (i === this.activeLayer ? ' selected' : '');
            row.innerHTML = `
                <span class="eye ${l.visible ? '' : 'off'}">${icon('eye', 12)}</span>
                <span class="lname">${escapeHtml(l.name)}</span>
                <input type="range" min="0" max="100" value="${Math.round(l.opacity * 100)}" style="width:64px;">`;
            row.querySelector('.eye').addEventListener('click', () => { l.visible = !l.visible; this.renderLayers(); this.render(); });
            row.querySelector('input').addEventListener('input', ev => { l.opacity = +ev.target.value / 100; this.render(); });
            row.addEventListener('click', ev => {
                if (ev.target.tagName === 'INPUT') return;
                this.activeLayer = i;
                this.renderLayers();
            });
            host.appendChild(row);
        }
    }

    // -------------------------------------------------------------- picking
    toImage(ev) {
        const rect = this.composite.getBoundingClientRect();
        return {
            x: Math.floor((ev.clientX - rect.left) / this.zoom),
            y: Math.floor((ev.clientY - rect.top) / this.zoom),
        };
    }

    activeCtx() { return this.doc.layers[this.activeLayer].canvas.getContext('2d'); }

    bindCanvasEvents() {
        const el = this.composite;
        let painting = false;
        let last = null;
        let panStart = null;

        const commitShape = (a, b) => {
            const ctx = this.activeCtx();
            ctx.save();
            ctx.globalCompositeOperation = this.tool === 'rect-f' || this.tool === 'ellipse-f' ? 'source-over' : 'source-over';
            ctx.globalAlpha = this.opacity;
            ctx.strokeStyle = this.color;
            ctx.fillStyle = this.color;
            ctx.lineWidth = this.brushSize;
            ctx.lineCap = 'round'; ctx.lineJoin = 'round';
            ctx.beginPath();
            if (this.tool === 'line') {
                ctx.moveTo(a.x + 0.5, a.y + 0.5);
                ctx.lineTo(b.x + 0.5, b.y + 0.5);
                ctx.stroke();
            } else if (this.tool === 'rect') {
                ctx.strokeRect(Math.min(a.x, b.x) + 0.5, Math.min(a.y, b.y) + 0.5,
                    Math.abs(b.x - a.x), Math.abs(b.y - a.y));
            } else if (this.tool === 'ellipse') {
                ctx.ellipse((a.x + b.x) / 2 + 0.5, (a.y + b.y) / 2 + 0.5,
                    Math.abs(b.x - a.x) / 2, Math.abs(b.y - a.y) / 2, 0, 0, Math.PI * 2);
                ctx.stroke();
            }
            ctx.restore();
        };

        el.addEventListener('mousedown', ev => {
            const pt = this.toImage(ev);
            if (this.tool === 'pan' || this.spaceDown) {
                panStart = { x: ev.clientX, y: ev.clientY, left: this.wrap.scrollLeft, top: this.wrap.scrollTop };
                ev.preventDefault();
                return;
            }
            if (this.tool === 'picker') {
                this.pickColor(pt);
                return;
            }
            if (this.tool === 'fill') {
                this.pushHistory();
                floodFill(this.activeCtx(), this.doc.width, this.doc.height, pt, hexToRgba(this.color), this.opacity);
                this.render();
                return;
            }
            if (['line', 'rect', 'ellipse'].includes(this.tool)) {
                painting = true;
                this.shapeStart = pt;
                return;
            }
            // brush / eraser
            this.pushHistory();
            painting = true;
            last = pt;
            this.stamp(pt);
            this.render();
        });

        window.addEventListener('mousemove', ev => {
            const pt = this.toImage(ev);
            if (this.doc) this.posEl.textContent = `${pt.x}, ${pt.y}`;
            if (panStart) {
                this.wrap.scrollLeft = panStart.left - (ev.clientX - panStart.x);
                this.wrap.scrollTop = panStart.top - (ev.clientY - panStart.y);
                return;
            }
            if (!painting) return;
            if (['line', 'rect', 'ellipse'].includes(this.tool)) {
                // Preview on an offscreen canvas.
                if (!this.previewCanvas || this.previewCanvas.width !== this.doc.width) {
                    this.previewCanvas = document.createElement('canvas');
                    this.previewCanvas.width = this.doc.width;
                    this.previewCanvas.height = this.doc.height;
                }
                const pctx = this.previewCanvas.getContext('2d');
                pctx.clearRect(0, 0, this.doc.width, this.doc.height);
                const save = this.activeCtx.bind(this);
                // Draw the shape preview directly into the preview canvas.
                const ctx = pctx;
                ctx.save();
                ctx.globalAlpha = this.opacity;
                ctx.strokeStyle = this.color;
                ctx.lineWidth = this.brushSize;
                ctx.lineCap = 'round';
                ctx.beginPath();
                if (this.tool === 'line') {
                    ctx.moveTo(this.shapeStart.x + 0.5, this.shapeStart.y + 0.5);
                    ctx.lineTo(pt.x + 0.5, pt.y + 0.5);
                    ctx.stroke();
                } else if (this.tool === 'rect') {
                    ctx.strokeRect(Math.min(this.shapeStart.x, pt.x) + 0.5, Math.min(this.shapeStart.y, pt.y) + 0.5,
                        Math.abs(pt.x - this.shapeStart.x), Math.abs(pt.y - this.shapeStart.y));
                } else {
                    ctx.ellipse((this.shapeStart.x + pt.x) / 2, (this.shapeStart.y + pt.y) / 2,
                        Math.abs(pt.x - this.shapeStart.x) / 2, Math.abs(pt.y - this.shapeStart.y) / 2, 0, 0, Math.PI * 2);
                    ctx.stroke();
                }
                ctx.restore();
                this.preview = true;
                this.render();
            } else {
                this.strokeTo(last, pt);
                last = pt;
                this.render();
            }
        });

        window.addEventListener('mouseup', ev => {
            if (panStart) { panStart = null; return; }
            if (!painting) return;
            painting = false;
            if (['line', 'rect', 'ellipse'].includes(this.tool)) {
                const pt = this.toImage(ev);
                commitShape(this.shapeStart, pt);
                this.preview = false;
                this.previewCanvas = null;
                this.render();
            }
        });
    }

    strokeTo(from, to) {
        const dist = Math.hypot(to.x - from.x, to.y - from.y);
        const steps = Math.max(1, Math.ceil(dist / Math.max(1, this.brushSize * 0.25)));
        for (let i = 1; i <= steps; ++i) {
            this.stamp({
                x: from.x + (to.x - from.x) * i / steps,
                y: from.y + (to.y - from.y) * i / steps,
            });
        }
    }

    stamp(pt) {
        const ctx = this.activeCtx();
        const r = Math.max(0.5, this.brushSize / 2);
        ctx.save();
        ctx.globalCompositeOperation = this.tool === 'eraser' ? 'destination-out' : 'source-over';
        ctx.globalAlpha = this.opacity;
        if (this.hardness >= 0.99 || this.brushSize <= 2) {
            ctx.fillStyle = this.color;
            ctx.beginPath();
            ctx.arc(pt.x, pt.y, r, 0, Math.PI * 2);
            ctx.fill();
        } else {
            const [pr, pg, pb] = hexToRgba(this.color).map(v => v * 255);
            const grad = ctx.createRadialGradient(pt.x, pt.y, r * this.hardness * 0.9, pt.x, pt.y, r);
            grad.addColorStop(0, `rgba(${pr | 0},${pg | 0},${pb | 0},1)`);
            grad.addColorStop(1, `rgba(${pr | 0},${pg | 0},${pb | 0},0)`);
            ctx.fillStyle = grad;
            ctx.beginPath();
            ctx.arc(pt.x, pt.y, r, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.restore();
    }

    pickColor(pt) {
        const data = this.activeCtx().getImageData(clamp(pt.x, 0, this.doc.width - 1), clamp(pt.y, 0, this.doc.height - 1), 1, 1).data;
        const hex = '#' + [data[0], data[1], data[2]].map(v => v.toString(16).padStart(2, '0')).join('');
        this.setColor(hex, true);
    }

    // ---------------------------------------------------------------- color
    setColor(hex, fromPicker) {
        if (!/^#[0-9a-fA-F]{6}$/.test(hex)) return;
        this.color = hex;
        this.colorInput.value = hex;
        this.hexInput.value = hex.toUpperCase();
        const [r, g, b] = hexToRgba(hex).map(v => Math.round(v * 255));
        this.hsv = rgbToHsv(r, g, b);
        this.renderPicker();
    }

    renderPicker() {
        // SV square.
        const sv = this.svEl;
        sv.innerHTML = '';
        const cvs = document.createElement('canvas');
        cvs.width = 196; cvs.height = 116;
        cvs.style.width = '100%'; cvs.style.height = '116px';
        const ctx = cvs.getContext('2d');
        const [hr, hg, hb] = hsvToRgb(this.hsv.h, 1, 1).map(v => Math.round(v * 255));
        ctx.fillStyle = `rgb(${hr},${hg},${hb})`;
        ctx.fillRect(0, 0, 196, 116);
        const gradW = ctx.createLinearGradient(0, 0, 196, 0);
        gradW.addColorStop(0, 'rgba(255,255,255,1)');
        gradW.addColorStop(1, 'rgba(255,255,255,0)');
        ctx.fillStyle = gradW;
        ctx.fillRect(0, 0, 196, 116);
        const gradB = ctx.createLinearGradient(0, 0, 0, 116);
        gradB.addColorStop(0, 'rgba(0,0,0,0)');
        gradB.addColorStop(1, 'rgba(0,0,0,1)');
        ctx.fillStyle = gradB;
        ctx.fillRect(0, 0, 196, 116);
        // Cursor.
        const cx = this.hsv.s * 196, cy = (1 - this.hsv.v) * 116;
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.arc(cx, cy, 5, 0, Math.PI * 2);
        ctx.stroke();
        ctx.strokeStyle = 'rgba(0,0,0,0.6)';
        ctx.lineWidth = 1;
        ctx.stroke();
        sv.appendChild(cvs);
        sv.onmousedown = ev => {
            const rect = cvs.getBoundingClientRect();
            const move = e => {
                this.hsv.s = clamp((e.clientX - rect.left) / rect.width, 0, 1);
                this.hsv.v = clamp(1 - (e.clientY - rect.top) / rect.height, 0, 1);
                this.applyHsv();
            };
            move(ev);
            const up = () => { window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); };
            window.addEventListener('mousemove', move);
            window.addEventListener('mouseup', up);
        };

        // Hue strip.
        const hue = this.hueEl;
        hue.innerHTML = '';
        const hc = document.createElement('canvas');
        hc.width = 196; hc.height = 12;
        hc.style.width = '100%'; hc.style.height = '12px';
        const hctx = hc.getContext('2d');
        for (let x = 0; x < 196; ++x) {
            const [r, g, b] = hsvToRgb(x / 196, 1, 1).map(v => Math.round(v * 255));
            hctx.fillStyle = `rgb(${r},${g},${b})`;
            hctx.fillRect(x, 0, 1, 12);
        }
        const hy = 2;
        hctx.fillStyle = '#fff';
        hctx.fillRect(this.hsv.h * 196 - 1, hy, 3, 12);
        hctx.strokeStyle = '#000';
        hctx.strokeRect(this.hsv.h * 196 - 1.5, hy - 0.5, 4, 13);
        hue.appendChild(hc);
        hue.onmousedown = ev => {
            const rect = hc.getBoundingClientRect();
            const move = e => {
                this.hsv.h = clamp((e.clientX - rect.left) / rect.width, 0, 1);
                this.applyHsv();
            };
            move(ev);
            const up = () => { window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); };
            window.addEventListener('mousemove', move);
            window.addEventListener('mouseup', up);
        };
    }

    applyHsv() {
        const [r, g, b] = hsvToRgb(this.hsv.h, this.hsv.s, this.hsv.v).map(v => Math.round(v * 255));
        const hex = '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
        this.color = hex;
        this.colorInput.value = hex;
        this.hexInput.value = hex.toUpperCase();
        this.renderPicker();
    }

    // ------------------------------------------------------------------ i/o
    newImageDialog() {
        import('./ui/dialogs.js').then(({ openDialog }) => {
            openDialog(t('imged.newImage'), (body, close, actions) => {
                body.innerHTML = `
                    <div class="form-row"><label>Width</label><input type="text" id="ie-w" value="${this.doc?.width || 256}"></div>
                    <div class="form-row"><label>Height</label><input type="text" id="ie-h" value="${this.doc?.height || 256}"></div>
                    <div class="form-row"><label>Background</label>
                        <select id="ie-bg"><option value="white">White</option><option value="transparent">Transparent</option><option value="black">Black</option></select></div>`;
                addButton(actions, t('common.create'), () => {
                    const w = clamp(parseInt(body.querySelector('#ie-w').value) || 256, 1, 4096);
                    const h = clamp(parseInt(body.querySelector('#ie-h').value) || 256, 1, 4096);
                    const bg = body.querySelector('#ie-bg').value;
                    close(null);
                    this.newDocument(w, h, bg !== 'transparent');
                    if (bg === 'black') {
                        const ctx = this.doc.layers[0].canvas.getContext('2d');
                        ctx.fillStyle = '#000';
                        ctx.fillRect(0, 0, w, h);
                        this.render();
                    }
                }, true);
                addButton(actions, t('dialog.cancel'), () => close(null));
            });
        });
    }

    async openDialog() {
        const file = await ipc.pickOpenFile({
            filters: [{ name: 'Images', extensions: ['png', 'jpg', 'jpeg', 'bmp'] }],
            defaultPath: path.join(editor.project?.path || '.', 'Assets', 'Textures'),
        });
        if (file) this.loadFile(file);
    }

    async save(saveAs) {
        let target = this.filePath;
        if (saveAs || !target) {
            const suggested = path.join(editor.project?.path || '.', 'Assets', 'Textures',
                path.basename(this.filePath || 'texture.png'));
            target = await ipc.pickSaveFile({
                filters: [{ name: 'PNG', extensions: ['png'] }],
                defaultPath: suggested,
            });
            if (!target) return;
            if (!target.toLowerCase().endsWith('.png')) target += '.png';
            this.filePath = target;
        }
        const url = this.compositeToDataURL();
        const base64 = url.split(',')[1];
        fs.writeFileSync(target, Buffer.from(base64, 'base64'));
        emit('asset-changed', target);
    }

    compositeToDataURL() {
        const c = document.createElement('canvas');
        c.width = this.doc.width; c.height = this.doc.height;
        const ctx = c.getContext('2d');
        for (const l of this.doc.layers) {
            if (!l.visible) continue;
            ctx.globalAlpha = l.opacity;
            ctx.drawImage(l.canvas, 0, 0);
        }
        return c.toDataURL('image/png');
    }

    // ------------------------------------------------------------ normal map
    normalMapDialog() {
        import('./ui/dialogs.js').then(({ openDialog }) => {
            openDialog(t('imged.normalMap.title'), (body, close, actions) => {
                body.innerHTML = `
                    <div class="form-row"><label>${t('imged.normalMap.source')}</label>
                        <select id="nm-src"><option value="luma">${t('imged.normalMap.luma')}</option><option value="alpha">${t('imged.normalMap.alpha')}</option></select></div>
                    <div class="form-row"><label>${t('imged.normalMap.strength')}</label>
                        <input type="range" id="nm-strength" min="1" max="20" value="6" style="width:100%;"></div>
                    <div class="form-row"><label>${t('imged.normalMap.invertX')}</label><input type="checkbox" id="nm-ix"></div>
                    <div class="form-row"><label>${t('imged.normalMap.invertY')}</label><input type="checkbox" id="nm-iy"></div>
                    <div class="form-hint">${t('imged.normalMap.hint')}</div>`;
                addButton(actions, t('imged.normalMap.generate'), () => {
                    const source = body.querySelector('#nm-src').value;
                    const strength = +body.querySelector('#nm-strength').value;
                    const invX = body.querySelector('#nm-ix').checked;
                    const invY = body.querySelector('#nm-iy').checked;
                    close(null);
                    this.generateNormalMap(source, strength, invX, invY);
                }, true);
                addButton(actions, t('dialog.cancel'), () => close(null));
            });
        });
    }

    generateNormalMap(source, strength, invertX, invertY) {
        const { width: w, height: h } = this.doc;
        // Height field from the composite.
        const src = document.createElement('canvas');
        src.width = w; src.height = h;
        const sctx = src.getContext('2d');
        for (const l of this.doc.layers) {
            if (!l.visible) continue;
            sctx.globalAlpha = l.opacity;
            sctx.drawImage(l.canvas, 0, 0);
        }
        const data = sctx.getImageData(0, 0, w, h).data;
        const height = new Float32Array(w * h);
        for (let i = 0; i < w * h; ++i) {
            const o = i * 4;
            height[i] = source === 'alpha'
                ? data[o + 3] / 255
                : (0.2126 * data[o] + 0.7152 * data[o + 1] + 0.0722 * data[o + 2]) / 255;
        }

        const out = document.createElement('canvas');
        out.width = w; out.height = h;
        const octx = out.getContext('2d');
        const img = octx.createImageData(w, h);
        const at = (x, y) => height[((y + h) % h) * w + ((x + w) % w)];
        for (let y = 0; y < h; ++y) {
            for (let x = 0; x < w; ++x) {
                // Sobel.
                const dx =
                    (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) -
                    (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
                const dy =
                    (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) -
                    (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
                let nx = -dx * strength * 0.25, ny = -dy * strength * 0.25, nz = 1;
                if (invertX) nx = -nx;
                if (invertY) ny = -ny;
                const len = Math.hypot(nx, ny, nz) || 1;
                const o = (y * w + x) * 4;
                img.data[o] = Math.round((nx / len * 0.5 + 0.5) * 255);
                img.data[o + 1] = Math.round((ny / len * 0.5 + 0.5) * 255);
                img.data[o + 2] = Math.round((nz / len * 0.5 + 0.5) * 255);
                img.data[o + 3] = 255;
            }
        }
        octx.putImageData(img, 0, 0);
        this.pushHistory();
        this.doc.layers.push({ name: 'Normal Map', canvas: out, visible: true, opacity: 1 });
        this.activeLayer = this.doc.layers.length - 1;
        this.renderLayers();
        this.render();
    }
}

// ---------------------------------------------------------------------------
function addButton(actions, label, onClick, primary) {
    const btn = document.createElement('button');
    btn.className = 'btn' + (primary ? ' primary' : '');
    btn.textContent = label;
    btn.addEventListener('click', onClick);
    actions.appendChild(btn);
}

function floodFill(ctx, w, h, start, rgba, opacity) {
    const img = ctx.getImageData(0, 0, w, h);
    const d = img.data;
    const idx = (x, y) => (y * w + x) * 4;
    const sx = clamp(start.x, 0, w - 1), sy = clamp(start.y, 0, h - 1);
    const target = d.slice(idx(sx, sy), idx(sx, sy) + 4);
    const tol = 24;
    const match = o =>
        Math.abs(d[o] - target[0]) <= tol && Math.abs(d[o + 1] - target[1]) <= tol &&
        Math.abs(d[o + 2] - target[2]) <= tol && Math.abs(d[o + 3] - target[3]) <= tol;
    const stack = [[sx, sy]];
    const seen = new Uint8Array(w * h);
    const [fr, fg, fb] = rgba;
    const fa = opacity;
    while (stack.length) {
        const [x, y] = stack.pop();
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        const p = y * w + x;
        if (seen[p]) continue;
        seen[p] = 1;
        const o = p * 4;
        if (!match(o)) continue;
        d[o] = Math.round(fr * 255 * fa + d[o] * (1 - fa));
        d[o + 1] = Math.round(fg * 255 * fa + d[o + 1] * (1 - fa));
        d[o + 2] = Math.round(fb * 255 * fa + d[o + 2] * (1 - fa));
        d[o + 3] = Math.max(d[o + 3], Math.round(fa * 255));
        stack.push([x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]);
    }
    ctx.putImageData(img, 0, 0);
}

function hexToRgba(hex) {
    const v = parseInt(hex.slice(1), 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255, 1];
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
function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
