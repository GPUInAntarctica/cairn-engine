// Cairn Engine editor - console panel.
'use strict';

import { engineInstance } from '../state.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';

const LEVELS = ['info', 'warning', 'error'];

export class ConsolePanel {
    constructor(pageEl) {
        this.pageEl = pageEl;
        this.entries = [];
        this.filters = { info: true, warning: true, error: true };
        pageEl.innerHTML = `
            <div class="console-toolbar">
                <div class="console-filter">
                    <span style="display:flex; align-items:center; gap:4px;">${icon('console', 12)} ${t('panel.console')}</span>
                </div>
                <div class="console-filter">
                    ${LEVELS.map(l => `<label><input type="checkbox" data-f="${l}" ${this.filters[l] ? 'checked' : ''}> ${t('console.filter.' + l)}</label>`).join('')}
                </div>
                <div style="flex:1;"></div>
                <button class="btn" style="height:20px; padding:0 8px; font-size:11px;">${t('console.clear')}</button>
            </div>
            <div class="console-list"><div class="muted" style="padding:12px;">${t('console.empty')}</div></div>`;
        this.list = pageEl.querySelector('.console-list');
        pageEl.querySelectorAll('input[data-f]').forEach(cb => {
            cb.addEventListener('change', () => {
                this.filters[cb.dataset.f] = cb.checked;
                this.render();
            });
        });
        pageEl.querySelector('button').addEventListener('click', () => {
            this.entries = [];
            this.render();
        });
        this.autoScroll = true;
        this.list.addEventListener('scroll', () => {
            this.autoScroll = this.list.scrollTop + this.list.clientHeight >= this.list.scrollHeight - 30;
        });
    }

    pump() {
        const logs = engineInstance().pollLogs();
        if (logs.length) {
            for (const l of logs) {
                this.entries.push({ level: LEVELS[l.level] || 'info', text: l.text, time: new Date() });
            }
            if (this.entries.length > 2000) this.entries.splice(0, this.entries.length - 2000);
            this.render();
        }
    }

    render() {
        this.list.innerHTML = '';
        let last = null;
        for (const e of this.entries) {
            if (!this.filters[e.level]) continue;
            const row = document.createElement('div');
            row.className = `log-row log-${e.level}`;
            const mark = e.level === 'error' ? '✕' : e.level === 'warning' ? '!' : '·';
            const time = `${String(e.time.getHours()).padStart(2, '0')}:${String(e.time.getMinutes()).padStart(2, '0')}:${String(e.time.getSeconds()).padStart(2, '0')}`;
            const dup = last && last.text === e.text && last.level === e.level ? (last.count = (last.count || 1) + 1) : null;
            row.innerHTML = `<span class="lvl">${mark}</span><span class="time">${time}</span><span class="msg">${escapeHtml(e.text)}</span>`
                + (dup ? `<span class="muted">×${dup}</span>` : '');
            this.list.appendChild(row);
            last = e;
        }
        if (!this.list.children.length) {
            this.list.innerHTML = `<div class="muted" style="padding:12px;">${t('console.empty')}</div>`;
        } else if (this.autoScroll) {
            this.list.scrollTop = this.list.scrollHeight;
        }
    }

    clear() { this.entries = []; this.render(); }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
