// Cairn Engine editor - center tab system (Scene / Code / Image Editor).
'use strict';

import { icon } from '../icons.js';

// pages: Map<id, {label, icon, closeable, pageEl, onClose, dirty}>
export class TabHost {
    constructor(hostEl) {
        this.hostEl = hostEl;
        this.pages = new Map();
        this.active = null;
        this.onActivate = null;
        hostEl.innerHTML = `
            <div class="tab-strip" role="tablist"></div>
            <div class="tab-content-host"></div>`;
        this.strip = hostEl.querySelector('.tab-strip');
        this.content = hostEl.querySelector('.tab-content-host');

        // Two delegated listeners for the whole strip instead of two per tab,
        // re-registered on every rebuild.
        this.strip.addEventListener('click', ev => {
            const close = ev.target.closest('.close');
            const tab = ev.target.closest('.tab');
            if (!tab) return;
            const id = tab.dataset.id;
            if (close) { ev.stopPropagation(); this.close(id); return; }
            this.activate(id);
        });
    }

    addPage(id, { label, icon: ic, closeable = true, build }) {
        if (this.pages.has(id)) {
            this.activate(id);
            return this.pages.get(id);
        }
        const pageEl = document.createElement('div');
        pageEl.className = 'tab-page';
        this.content.appendChild(pageEl);
        const page = { id, label, icon: ic, closeable, pageEl, onClose: null, dirty: false };
        if (build) build(pageEl, page);
        this.pages.set(id, page);
        this.renderStrip();
        this.activate(id);
        return page;
    }

    setLabel(id, label) {
        const page = this.pages.get(id);
        if (!page || page.label === label) return;
        page.label = label;
        this.renderStrip();
    }

    // Unsaved marker. The dot takes the close affordance's place rather than
    // sitting beside it, so hover and dirty never compete for the same pixels.
    setDirty(id, dirty) {
        const page = this.pages.get(id);
        if (!page || page.dirty === !!dirty) return;
        page.dirty = !!dirty;
        this.renderStrip();
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
        // Reuse the existing tab nodes; only the class, label and dirty dot
        // change. Called on every scene mutation, so rebuilding here used to
        // tear down and re-create the whole strip mid-drag.
        const seen = new Set();
        for (const [id, page] of this.pages) {
            seen.add(id);
            let tab = this.strip.querySelector(`.tab[data-id="${cssEscape(id)}"]`);
            if (!tab) {
                tab = document.createElement('div');
                tab.className = 'tab';
                tab.dataset.id = id;
                tab.setAttribute('role', 'tab');
                tab.tabIndex = 0;
                tab.innerHTML = '<span class="tab-icon"></span><span class="tab-label"></span>' +
                    '<span class="close"></span>';
            }
            const active = id === this.active;
            tab.classList.toggle('active', active);
            tab.setAttribute('aria-selected', active ? 'true' : 'false');
            const iconEl = tab.querySelector('.tab-icon');
            const iconName = page.icon || 'scene';
            if (iconEl.dataset.icon !== iconName) {
                iconEl.dataset.icon = iconName;
                iconEl.innerHTML = icon(iconName);
            }
            const labelEl = tab.querySelector('.tab-label');
            if (labelEl.textContent !== page.label) labelEl.textContent = page.label;
            tab.title = page.label;
            tab.querySelector('.close').innerHTML = page.closeable && !page.dirty ? icon('close') : '';
            tab.classList.toggle('dirty', !!page.dirty);
            this.strip.appendChild(tab);
        }
        for (const tab of [...this.strip.children]) {
            if (!seen.has(tab.dataset.id)) tab.remove();
        }
    }
}

// Tab ids embed absolute paths (code:/image:/…), which can contain characters
// that are not valid in a bare attribute selector.
function cssEscape(s) {
    return String(s).replace(/["\\]/g, '\\$&');
}
