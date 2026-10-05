// Cairn Engine editor - console panel.
'use strict';

import { engineInstance } from '../state.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';

const LEVELS = ['info', 'warning', 'error'];
const MARKS = { error: '✕', warning: '!', info: '·' };
const MAX_ENTRIES = 2000;

export class ConsolePanel {
    constructor(pageEl) {
        this.pageEl = pageEl;
        this.entries = [];
        this.filters = { info: true, warning: true, error: true };
        pageEl.innerHTML = `
            <div class="console-toolbar">
                <span class="console-title">${icon('console')}<span>${escapeHtml(t('panel.console'))}</span></span>
                <span class="console-filter">
                    ${LEVELS.map(l => `<label><input type="checkbox" data-f="${l}" ${this.filters[l] ? 'checked' : ''}>${escapeHtml(t('console.filter.' + l))}</label>`).join('')}
                </span>
                <span class="console-spacer"></span>
                <button class="btn btn-sm">${escapeHtml(t('console.clear'))}</button>
            </div>
            <div class="console-list" role="log"></div>`;
        this.list = pageEl.querySelector('.console-list');
        pageEl.querySelectorAll('input[data-f]').forEach(cb => {
            cb.addEventListener('change', () => {
                this.filters[cb.dataset.f] = cb.checked;
                this.render();
            });
        });
        pageEl.querySelector('button').addEventListener('click', () => this.clear());
        this.autoScroll = true;
        this.list.addEventListener('scroll', () => {
            // Re-arm from either edge: scrolling back to the bottom turns
            // auto-scroll back on, which the previous one-way test could not do.
            const gap = this.list.scrollHeight - this.list.scrollTop - this.list.clientHeight;
            this.autoScroll = gap < 24;
        });
        this.render();
    }

    pump() {
        const logs = engineInstance().pollLogs();
        if (!logs.length) return;
        for (const l of logs) {
            const last = this.entries[this.entries.length - 1];
            const text = l.text;
            // Collapse at push time. Counting during render stored the tally on
            // the previous row, so a run of identical lines printed x2, x3, x4
            // down the list instead of one row reading x5.
            if (last && last.text === text && last.level === l.level && last.count) {
                last.count++;
                last.time = new Date();
                continue;
            }
            this.entries.push({ level: LEVELS[l.level] || 'info', text, time: new Date(), count: 0 });
        }
        if (this.entries.length > MAX_ENTRIES) this.entries.splice(0, this.entries.length - MAX_ENTRIES);
        this.render();
    }

    render() {
        const rows = [];
        let last = null;
        for (const e of this.entries) {
            if (!this.filters[e.level]) { last = null; continue; }
            const repeat = last && last.text === e.text && last.level === e.level;
            rows.push(
                '<div class="log-row log-' + e.level + '">' +
                    '<span class="lvl">' + MARKS[e.level] + '</span>' +
                    '<span class="time">' + hhmss(e.time) + '</span>' +
                    '<span class="msg">' + escapeHtml(e.text) + '</span>' +
                    (repeat ? '<span class="count">×' + e.count + '</span>' : '') +
                '</div>');
            last = e;
        }
        if (!rows.length) {
            this.list.innerHTML = '<div class="empty-line">' + escapeHtml(t('console.empty')) + '</div>';
            return;
        }
        // One innerHTML write instead of createElement + innerHTML per row.
        this.list.innerHTML = rows.join('');
        if (this.autoScroll) this.list.scrollTop = this.list.scrollHeight;
    }

    clear() { this.entries = []; this.render(); }
}

function hhmss(d) {
    return [d.getHours(), d.getMinutes(), d.getSeconds()]
        .map(v => String(v).padStart(2, '0')).join(':');
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}