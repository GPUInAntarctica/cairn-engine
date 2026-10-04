// Cairn Engine editor - status bar.
'use strict';

import { t } from '../i18n.js';

export class StatusBar {
    constructor(hostEl) {
        this.hostEl = hostEl;
        hostEl.innerHTML = `
            <span id="sb-left">${t('status.ready')}</span>
            <span id="sb-scene" class="muted"></span>
            <span class="spacer"></span>
            <span class="stat"><span class="muted">FPS</span><b id="sb-fps">–</b></span>
            <span class="stat"><span class="muted">Draw</span><b id="sb-draw">–</b></span>
            <span class="stat"><span class="muted">Tris</span><b id="sb-tris">–</b></span>
            <span class="stat"><b id="sb-selection"></b></span>`;
    }

    setMessage(msg) { this.hostEl.querySelector('#sb-left').textContent = msg; }

    update({ sceneLabel, dirty, playing, paused, fps, stats, selection }) {
        const q = sel => this.hostEl.querySelector(sel);
        q('#sb-scene').textContent = sceneLabel + (dirty ? '  ●' : '');
        q('#sb-left').textContent = playing ? (paused ? t('status.paused') : t('status.playing')) : t('status.ready');
        q('#sb-fps').textContent = Math.round(fps);
        q('#sb-draw').textContent = stats.drawCalls;
        q('#sb-tris').textContent = stats.triangles;
        q('#sb-selection').textContent = selection ? selection.name : '';
    }
}
