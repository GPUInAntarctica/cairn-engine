// Cairn Engine editor - center tab system (Scene / Code / Image Editor).
'use strict';

import { icon } from '../icons.js';
import { t } from '../i18n.js';

// pages: Map<id, {label, icon, closeable, element, onClose}>
export class TabHost {
    constructor(hostEl) {
        this.hostEl = hostEl;
        this.pages = new Map();
        this.active = null;
        hostEl.innerHTML = `
            <div class="tab-strip"></div>
            <div class="tab-content-host"></div>`;
        this.strip = hostEl.querySelector('.tab-strip');
        this.content = hostEl.querySelector('.tab-content-host');
    }

    addPage(id, { label, icon: ic, closeable = true, build }) {
        if (this.pages.has(id)) {
            this.activate(id);
            return this.pages.get(id);
        }
        const pageEl = document.createElement('div');
        pageEl.className = 'tab-page';
        this.content.appendChild(pageEl);
        const page = { id, label, icon: ic, closeable, pageEl, onClose: null };
        if (build) build(pageEl, page);
        this.pages.set(id, page);
        this.renderStrip();
        this.activate(id);
        return page;
    }

    activate(id) {
        if (!this.pages.has(id)) return;
        this.active = id;
        for (const [pid, page] of this.pages) {
            page.pageEl.classList.toggle('active', pid === id);
        }
        this.renderStrip();
        if (this.onActivate) this.onActivate(id);
    }

    close(id) {
        const page = this.pages.get(id);
        if (!page || !page.closeable) return;
        if (page.onClose && page.onClose() === false) return;
        page.pageEl.remove();
        this.pages.delete(id);
        if (this.active === id) {
            const next = this.pages.has('scene') ? 'scene' : this.pages.keys().next().value;
            if (next) this.activate(next);
        }
        this.renderStrip();
    }

    renderStrip() {
        this.strip.innerHTML = '';
        for (const [id, page] of this.pages) {
            const tab = document.createElement('div');
            tab.className = 'tab' + (id === this.active ? ' active' : '');
            tab.innerHTML = `${icon(page.icon || 'scene')}<span>${page.label}</span>` +
                (page.closeable ? `<span class="close">${icon('close', 10)}</span>` : '');
            tab.addEventListener('click', () => this.activate(id));
            if (page.closeable) {
                tab.querySelector('.close').addEventListener('click', ev => {
                    ev.stopPropagation();
                    this.close(id);
                });
            }
            this.strip.appendChild(tab);
        }
    }
}
