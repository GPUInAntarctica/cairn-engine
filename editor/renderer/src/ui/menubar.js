// Cairn Engine editor - menu bar.
// Root menus share the context menu's geometry and keyboard contract; the only
// difference is that they anchor under a bar rather than at a point.
'use strict';

import { t } from '../i18n.js';
import { icon } from '../icons.js';

const PAGE_MARGIN = 8;

export class MenuBar {
    constructor(hostEl, actions) {
        this.hostEl = hostEl;
        this.actions = actions;
        this.openMenu = null;
        this.render();
        window.addEventListener('mousedown', ev => {
            if (!ev.target.closest('.menu-dropdown') && !ev.target.closest('.menu-root')) this.closeAll();
        });
        window.addEventListener('keydown', ev => {
            if (ev.key === 'Escape' && this.openMenu !== null) { this.closeAll(); ev.stopPropagation(); }
        });
    }

    closeAll() {
        this.hostEl.querySelectorAll('.menu-root').forEach(el => {
            el.classList.remove('open');
            el.setAttribute('aria-expanded', 'false');
        });
        // Scoped to our own dropdowns: a bare '.menu-dropdown' query also
        // removed context menus, which the two modules opened independently.
        document.querySelectorAll('.menu-dropdown.menu').forEach(el => el.remove());
        this.openMenu = null;
    }

    render() {
        const defs = this.actions;
        this.hostEl.innerHTML = `
            <div class="menu-brand">${icon('logo', 15)}<span>Cairn</span></div>
            ${defs.map((menu, i) => `<div class="menu-root" data-i="${i}" role="menuitem" tabindex="0" aria-haspopup="true" aria-expanded="false">${escapeHtml(t(menu.label))}</div>`).join('')}`;

        this.hostEl.querySelectorAll('.menu-root').forEach(el => {
            el.addEventListener('click', ev => {
                ev.stopPropagation();
                const i = +el.dataset.i;
                if (this.openMenu === i) { this.closeAll(); return; }
                this.closeAll();
                this.open(el, defs[i]);
            });
            el.addEventListener('keydown', ev => {
                if (ev.key === 'Enter' || ev.key === ' ') {
                    ev.preventDefault();
                    this.closeAll();
                    this.open(el, defs[+el.dataset.i]);
                }
            });
            el.addEventListener('mouseenter', () => {
                // Hover-to-switch only while a menu is already open, so
                // sweeping the bar to find the File menu does not flicker.
                if (this.openMenu !== null && this.openMenu !== +el.dataset.i) {
                    this.closeAll();
                    this.open(el, defs[+el.dataset.i]);
                }
            });
        });
    }

    open(anchorEl, menu) {
        anchorEl.classList.add('open');
        anchorEl.setAttribute('aria-expanded', 'true');
        this.openMenu = +anchorEl.dataset.i;
        const dd = document.createElement('div');
        dd.className = 'menu-dropdown menu';
        dd.setAttribute('role', 'menu');
        const list = [];
        for (const item of menu.items) {
            if (item === '-') {
                dd.appendChild(Object.assign(document.createElement('div'), { className: 'menu-sep' }));
                continue;
            }
            const el = document.createElement('div');
            // Enabled is evaluated at open time so a menu opened while nothing
            // is selected shows its disabled entries greyed out.
            const disabled = item.enabled ? !item.enabled() : false;
            el.className = 'menu-item' + (disabled ? ' disabled' : '');
            el.setAttribute('role', 'menuitem');
            el.tabIndex = disabled ? -1 : 0;
            el.innerHTML = `<span>${escapeHtml(t(item.label))}</span>` +
                (item.shortcut ? `<span class="shortcut">${escapeHtml(item.shortcut)}</span>` : '');
            if (!disabled) {
                el.addEventListener('click', () => { this.closeAll(); item.run(); });
                el.addEventListener('keydown', ev => {
                    if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); this.closeAll(); item.run(); }
                });
            }
            dd.appendChild(el);
            if (!disabled) list.push(el);
        }
        const rect = anchorEl.getBoundingClientRect();
        document.body.appendChild(dd);
        // Clamp, and flip above the bar when the menu would run off-screen.
        const drect = dd.getBoundingClientRect();
        let left = rect.left;
        let top = rect.bottom;
        if (left + drect.width + PAGE_MARGIN > window.innerWidth) {
            left = Math.max(PAGE_MARGIN, window.innerWidth - drect.width - PAGE_MARGIN);
        }
        if (top + drect.height + PAGE_MARGIN > window.innerHeight) {
            top = Math.max(PAGE_MARGIN, rect.top - drect.height);
        }
        dd.style.left = left + 'px';
        dd.style.top = top + 'px';
        list[0]?.focus({ preventScroll: true });
    }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}