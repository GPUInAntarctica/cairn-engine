// Cairn Engine editor - viewport toolbar (tools + play controls + view modes).
'use strict';

import { t } from '../i18n.js';
import { icon } from '../icons.js';
import { editor } from '../state.js';
import { on } from '../events.js';

export class Toolbar {
    constructor(hostEl, viewport, handlers) {
        this.hostEl = hostEl;
        this.viewport = viewport;
        this.handlers = handlers;
        hostEl.innerHTML = `
            <div class="tb-group" id="tb-tools" role="group">
                <button class="tb-btn" data-tool="select" title="${escapeAttr(t('tool.select'))}" aria-pressed="false">${icon('select', 16)}</button>
                <button class="tb-btn" data-tool="move" title="${escapeAttr(t('tool.move'))}" aria-pressed="false">${icon('move', 16)}</button>
                <button class="tb-btn" data-tool="rotate" title="${escapeAttr(t('tool.rotate'))}" aria-pressed="false">${icon('rotate', 16)}</button>
                <button class="tb-btn" data-tool="scale" title="${escapeAttr(t('tool.scale'))}" aria-pressed="false">${icon('scale', 16)}</button>
            </div>
            <div class="tb-sep"></div>
            <div class="tb-group" id="tb-play" role="group">
                <button class="tb-btn" data-play="play" title="${escapeAttr(t('tool.play'))}">${icon('play', 16)}</button>
                <button class="tb-btn" data-play="pause" title="${escapeAttr(t('tool.pause'))}">${icon('pause', 16)}</button>
                <button class="tb-btn" data-play="step" title="${escapeAttr(t('tool.step'))}">${icon('step', 16)}</button>
                <button class="tb-btn" data-play="stop" title="${escapeAttr(t('tool.stop'))}">${icon('stop', 16)}</button>
            </div>
            <div class="tb-sep"></div>
            <div class="tb-group">
                <button class="tb-btn" id="tb-ortho" title="${escapeAttr(t('tool.ortho'))}" aria-pressed="false">${icon('cube', 16)}</button>
                <button class="tb-btn" id="tb-stats" title="${escapeAttr(t('menu.window.stats'))}" aria-pressed="false">${icon('console', 16)}</button>
            </div>`;

        // Bound once, against this toolbar's own subtree: the previous version
        // queried the whole document, so a second toolbar anywhere in the app
        // would have driven these buttons.
        this.toolBtns = [...hostEl.querySelectorAll('[data-tool]')];
        this.playBtns = {};
        hostEl.querySelectorAll('[data-play]').forEach(btn => {
            this.playBtns[btn.dataset.play] = btn;
            btn.addEventListener('click', () => handlers.playControl(btn.dataset.play));
        });
        this.orthoBtn = hostEl.querySelector('#tb-ortho');
        this.statsBtn = hostEl.querySelector('#tb-stats');
        this.toolBtns.forEach(btn => btn.addEventListener('click', () => this.setTool(btn.dataset.tool)));
        this.orthoBtn.addEventListener('click', () => {
            viewport.cam.ortho = !viewport.cam.ortho;
            this.refresh();
        });
        this.statsBtn.addEventListener('click', () => {
            viewport.showStats = !viewport.showStats;
            viewport.drawOverlay();
            this.refresh();
        });

        on('play-state', () => this.refresh());
        this.refresh();
    }

    setTool(tool) {
        editor.tool = tool;
        this.viewport.setTool(tool);
        this.refresh();
    }

    refresh() {
        for (const btn of this.toolBtns) {
            const on_ = btn.dataset.tool === editor.tool;
            btn.classList.toggle('active', on_);
            btn.setAttribute('aria-pressed', on_ ? 'true' : 'false');
        }
        for (const btn of Object.values(this.playBtns)) btn.classList.remove('active', 'playing');
        // Keyed by data-play rather than by position, so reordering the
        // controls cannot silently highlight the wrong one.
        if (editor.playing) {
            this.playBtns.play?.classList.add('playing');
            if (editor.paused) this.playBtns.pause?.classList.add('active');
        }
        this.orthoBtn.classList.toggle('active', this.viewport.cam.ortho);
        this.orthoBtn.setAttribute('aria-pressed', this.viewport.cam.ortho ? 'true' : 'false');
        this.statsBtn.classList.toggle('active', this.viewport.showStats);
        this.statsBtn.setAttribute('aria-pressed', this.viewport.showStats ? 'true' : 'false');
    }
}

function escapeAttr(s) {
    return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}