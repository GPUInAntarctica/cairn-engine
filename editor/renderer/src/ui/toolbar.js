// Cairn Engine editor - viewport toolbar (tools + play controls + view modes).
'use strict';

import { t } from '../i18n.js';
import { icon } from '../icons.js';
import { editor, selectEntity, entityList } from '../state.js';
import { emit } from '../events.js';

export class Toolbar {
    constructor(hostEl, viewport, handlers) {
        this.viewport = viewport;
        this.handlers = handlers;
        hostEl.innerHTML = `
            <div class="tb-group" id="tb-tools">
                <button class="tb-btn" data-tool="select" title="${t('tool.select')}">${icon('select')}</button>
                <button class="tb-btn" data-tool="move" title="${t('tool.move')}">${icon('move')}</button>
                <button class="tb-btn" data-tool="rotate" title="${t('tool.rotate')}">${icon('rotate')}</button>
                <button class="tb-btn" data-tool="scale" title="${t('tool.scale')}">${icon('scale')}</button>
            </div>
            <div class="tb-sep"></div>
            <div class="tb-group" id="tb-play">
                <button class="tb-btn" data-play="play" title="${t('tool.play')}">${icon('play')}</button>
                <button class="tb-btn" data-play="pause" title="${t('tool.pause')}">${icon('pause')}</button>
                <button class="tb-btn" data-play="step" title="${t('tool.step')}">${icon('step')}</button>
                <button class="tb-btn" data-play="stop" title="${t('tool.stop')}">${icon('stop')}</button>
            </div>
            <div class="tb-sep"></div>
            <div class="tb-group">
                <button class="tb-btn" id="tb-ortho" title="${t('tool.ortho')}">${icon('cube')}</button>
                <button class="tb-btn" id="tb-stats" title="${t('menu.window.stats')}">${icon('console')}</button>
            </div>`;

        hostEl.querySelectorAll('[data-tool]').forEach(btn => {
            btn.addEventListener('click', () => this.setTool(btn.dataset.tool));
        });
        hostEl.querySelectorAll('[data-play]').forEach(btn => {
            btn.addEventListener('click', () => handlers.playControl(btn.dataset.play));
        });
        hostEl.querySelector('#tb-ortho').addEventListener('click', () => {
            viewport.cam.ortho = !viewport.cam.ortho;
            this.refresh();
        });
        hostEl.querySelector('#tb-stats').addEventListener('click', () => {
            viewport.showStats = !viewport.showStats;
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
        const el = this.hostEl || this;
        document.querySelectorAll('#tb-tools .tb-btn').forEach(b =>
            b.classList.toggle('active', b.dataset.tool === editor.tool));
        const playBtns = document.querySelectorAll('#tb-play .tb-btn');
        playBtns.forEach(b => b.classList.remove('active', 'playing'));
        if (editor.playing) {
            const map = { play: 'play', pause: 'pause' };
            playBtns[0].classList.add('playing');
            if (editor.paused) playBtns[1].classList.add('active');
        }
        const orthoBtn = document.querySelector('#tb-ortho');
        if (orthoBtn) orthoBtn.classList.toggle('active', this.viewport.cam.ortho);
        const statsBtn = document.querySelector('#tb-stats');
        if (statsBtn) statsBtn.classList.toggle('active', this.viewport.showStats);
    }
}

import { on } from '../events.js';
