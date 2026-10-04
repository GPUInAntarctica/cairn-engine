// Cairn Engine editor - menu bar (HTML, localizable).
'use strict';

import { t } from '../i18n.js';
import { icon } from '../icons.js';

export class MenuBar {
    constructor(hostEl, actions) {
        this.hostEl = hostEl;
        this.actions = actions;
        this.openMenu = null;
        this.render();
        window.addEventListener('mousedown', ev => {
            if (!ev.target.closest('.menu-dropdown') && !ev.target.closest('.menu-root')) this.closeAll();
        });
    }

    closeAll() {
        this.hostEl.querySelectorAll('.menu-root').forEach(el => el.classList.remove('open'));
        document.querySelectorAll('.menu-dropdown').forEach(el => el.remove());
        this.openMenu = null;
    }

    render() {
        const defs = this.actions;
        this.hostEl.innerHTML = `
            <div class="menu-brand">${icon('logo', 15)} Cairn</div>
            ${defs.map((menu, i) => `<div class="menu-root" data-i="${i}">${t(menu.label)}</div>`).join('')}`;

        this.hostEl.querySelectorAll('.menu-root').forEach(el => {
            el.addEventListener('click', ev => {
                ev.stopPropagation();
                const i = +el.dataset.i;
                if (this.openMenu === i) { this.closeAll(); return; }
                this.closeAll();
                this.open(el, defs[i]);
            });
            el.addEventListener('mouseenter', () => {
                if (this.openMenu !== null) {
                    this.closeAll();
                    this.open(el, defs[+el.dataset.i]);
                }
            });
        });
    }

    open(anchorEl, menu) {
        anchorEl.classList.add('open');
        this.openMenu = +anchorEl.dataset.i;
        const dd = document.createElement('div');
        dd.className = 'menu-dropdown';
        for (const item of menu.items) {
            if (item === '-') {
                dd.appendChild(Object.assign(document.createElement('div'), { className: 'menu-sep' }));
                continue;
            }
            const el = document.createElement('div');
            const disabled = item.enabled ? !item.enabled() : false;
            el.className = 'menu-item' + (disabled ? ' disabled' : '');
            el.innerHTML = `<span>${t(item.label)}</span>${item.shortcut ? `<span class="shortcut">${item.shortcut}</span>` : ''}`;
            el.addEventListener('click', () => {
                if (disabled) return;
                this.closeAll();
                item.run();
            });
            dd.appendChild(el);
        }
        const rect = anchorEl.getBoundingClientRect();
        dd.style.left = rect.left + 'px';
        dd.style.top = rect.bottom + 'px';
        document.body.appendChild(dd);
    }
}
