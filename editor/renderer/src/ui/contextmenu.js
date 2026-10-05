// Cairn Engine editor - shared context menu.
//
// Overlays in this app share one geometry vocabulary with the menu bar:
// a 4px page margin, a 1px border and a tight ambient occlusion. Keyboard
// parity is complete - arrows walk the list, Right opens a submenu, Escape
// closes - because a context menu reached by right-click is still a menu.
'use strict';

import { t } from '../i18n.js';

const PAGE_MARGIN = 8;   // matches contextmenu / dialog scrim clearance

let closeActive = null;

export function showContextMenu(x, y, items) {
    closeContextMenu();
    const menu = document.createElement('div');
    menu.className = 'menu-dropdown ctx';
    menu.setAttribute('role', 'menu');
    menu.tabIndex = -1;

    const subs = [];
    const build = (list, container) => {
        for (const item of list) {
            if (item === '-') {
                container.appendChild(Object.assign(document.createElement('div'), { className: 'menu-sep' }));
                continue;
            }
            const el = document.createElement('div');
            el.className = 'menu-item';
            el.setAttribute('role', 'menuitem');
            el.tabIndex = -1;
            // One parse, one pass: the previous version assigned innerHTML and
            // then appended the submenu marker with a second '+='.
            el.innerHTML = `<span>${escapeHtml(t(item.label))}</span>` +
                (item.children ? '<span class="shortcut">▸</span>' : '');
            if (item.children) {
                el.addEventListener('mouseenter', () => openSub(item, el, container));
                el.addEventListener('focus', () => openSub(item, el, container));
            } else {
                el.addEventListener('click', () => { closeContextMenu(); item.run(); });
                el.addEventListener('keydown', ev => {
                    if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); closeContextMenu(); item.run(); }
                });
            }
            container.appendChild(el);
        }
    };

    function openSub(item, anchor, container) {
        for (const s of subs) s.remove();
        subs.length = 0;
        const sub = document.createElement('div');
        sub.className = 'menu-dropdown ctx submenu';
        sub.setAttribute('role', 'menu');
        const rect = anchor.getBoundingClientRect();
        // Flip to the left of the parent when there is no room on the right.
        const fits = rect.right + sub.offsetWidth + PAGE_MARGIN < window.innerWidth;
        sub.style.left = (fits ? rect.right - 4 : rect.left - sub.offsetWidth + 4) + 'px';
        sub.style.top = Math.min(rect.top, window.innerHeight - sub.offsetHeight - PAGE_MARGIN) + 'px';
        build(item.children, sub);
        document.body.appendChild(sub);
        subs.push(sub);
        const first = sub.querySelector('.menu-item');
        if (first) first.focus({ preventScroll: true });
    }

    build(items, menu);
    document.body.appendChild(menu);

    const rect = menu.getBoundingClientRect();
    menu.style.left = Math.max(PAGE_MARGIN, Math.min(x, window.innerWidth - rect.width - PAGE_MARGIN)) + 'px';
    menu.style.top = Math.max(PAGE_MARGIN, Math.min(y, window.innerHeight - rect.height - PAGE_MARGIN)) + 'px';

    const items$ = () => [...menu.querySelectorAll('.menu-item:not(.disabled)')];
    menu.addEventListener('keydown', ev => {
        const list = items$();
        const i = list.indexOf(document.activeElement);
        if (ev.key === 'Escape') { ev.preventDefault(); closeContextMenu(); return; }
        if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
            ev.preventDefault();
            const next = ev.key === 'ArrowDown'
                ? Math.min(list.length - 1, i + 1)
                : Math.max(0, i <= 0 ? 0 : i - 1);
            list[next]?.focus({ preventScroll: true });
        } else if (ev.key === 'ArrowLeft' && menu.parentElement === document.body && i > -1) {
            ev.preventDefault();
            closeContextMenu();
        } else if (ev.key === 'ArrowRight') {
            const el = document.activeElement;
            if (el && el.querySelector('.shortcut')) el.dispatchEvent(new Event('mouseenter'));
        }
    });
    menu.focus({ preventScroll: true });

    const onDocDown = ev => {
        if (menu.contains(ev.target)) return;
        closeContextMenu();
    };
    const onKey = ev => {
        if (ev.key === 'Escape') closeContextMenu();
    };
    // Deferred so the mousedown that opened the menu does not immediately
    // close it.
    setTimeout(() => {
        window.addEventListener('mousedown', onDocDown, { once: true });
        window.addEventListener('keydown', onKey, { once: true });
        window.addEventListener('blur', closeContextMenu, { once: true });
    }, 0);

    closeActive = () => {
        window.removeEventListener('mousedown', onDocDown);
        window.removeEventListener('keydown', onKey);
        window.removeEventListener('blur', closeContextMenu);
    };
}

export function closeContextMenu() {
    if (closeActive) { closeActive(); closeActive = null; }
    // Class-scoped, not #ctx-menu: submenus have no id, so the previous
    // id-scoped close left them orphaned in the document body.
    document.querySelectorAll('.menu-dropdown.ctx').forEach(el => el.remove());
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}