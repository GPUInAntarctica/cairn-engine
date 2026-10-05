// Cairn Engine editor - status bar.
//
// Called from the animation frame loop, so it writes nothing unless a value
// actually changed: assigning an identical string still dirties the node in
// Blink and would re-measure four spans sixty times a second.
//
// The left cluster has two independent slots rather than one. Transient
// messages and the play/ready state used to share #sb-left, so a status line
// like "Scripts built in 412 ms" was overwritten by the next frame and never
// became readable.
'use strict';

import { t } from '../i18n.js';

export class StatusBar {
    constructor(hostEl) {
        this.hostEl = hostEl;
        this.cache = new Map();
        hostEl.innerHTML = `
            <span id="sb-left" class="idle"></span>
            <span id="sb-state"></span>
            <span id="sb-scene"></span>
            <span id="sb-compiler" class="muted"></span>
            <span class="spacer"></span>
            <span id="sb-counters">
                <span class="stat"><span class="muted">${escapeHtml(t('status.fps'))}</span><b id="sb-fps">–</b></span>
                <span class="stat"><span class="muted">${escapeHtml(t('status.draw'))}</span><b id="sb-draw">–</b></span>
                <span class="stat"><span class="muted">${escapeHtml(t('status.tris'))}</span><b id="sb-tris">–</b></span>
            </span>
            <span class="stat"><b id="sb-selection"></b></span>`;
        this.els = {};
        for (const id of ['sb-left', 'sb-state', 'sb-scene', 'sb-compiler', 'sb-fps', 'sb-draw', 'sb-tris', 'sb-selection']) {
            this.els[id] = hostEl.querySelector('#' + id);
        }
    }

    // The detected C++ compiler is a standing fact about the environment, not
    // a transient event, so it gets a permanent low-emphasis slot rather than
    // the message slot - which used to leave the bar reading "Ready - g++
    // Ready" at every startup.
    setCompiler(name) {
        this.write('sb-compiler', name || '');
    }

    // Transient message slot. Independent of the play state, so it survives
    // until something explicitly clears it.
    setMessage(msg) {
        this.els['sb-left'].textContent = msg;
        this.els['sb-left'].classList.toggle('live', !!msg);
        this.els['sb-left'].classList.toggle('idle', !msg);
    }

    update({ sceneLabel, dirty, playing, paused, fps, stats, selection }) {
        const state = playing ? (paused ? t('status.paused') : t('status.playing')) : t('status.ready');
        this.write('sb-state', state);
        this.els['sb-state'].classList.toggle('playing', playing);
        this.write('sb-scene', sceneLabel);
        this.els['sb-scene'].classList.toggle('dirty', !!dirty);
        this.write('sb-fps', Math.round(fps));
        this.write('sb-draw', stats.drawCalls);
        this.write('sb-tris', stats.triangles);
        this.write('sb-selection', selection ? selection.name : '');
    }

    write(id, value) {
        const v = String(value);
        if (this.cache.get(id) === v) return;
        this.cache.set(id, v);
        this.els[id].textContent = v;
    }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}