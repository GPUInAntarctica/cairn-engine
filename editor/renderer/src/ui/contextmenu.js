// Cairn Engine editor - shared context menu.
'use strict';

export function showContextMenu(x, y, items) {
    closeContextMenu();
    const menu = document.createElement('div');
    menu.className = 'menu-dropdown';
    menu.id = 'ctx-menu';

    const build = (list, container) => {
        for (const item of list) {
            if (item === '-') {
                container.appendChild(Object.assign(document.createElement('div'), { className: 'menu-sep' }));
                continue;
            }
            const el = document.createElement('div');
            el.className = 'menu-item';
            el.innerHTML = `<span>${label(item.label)}</span>`;
            if (item.children) {
                el.innerHTML += `<span class="shortcut">▸</span>`;
                el.addEventListener('mouseenter', () => {
                    container.querySelectorAll('.menu-dropdown').forEach(m => m.remove());
                    const sub = document.createElement('div');
                    sub.className = 'menu-dropdown';
                    const rect = el.getBoundingClientRect();
                    sub.style.left = rect.right - 4 + 'px';
                    sub.style.top = rect.top + 'px';
                    build(item.children, sub);
                    document.body.appendChild(sub);
                });
            } else {
                el.addEventListener('click', () => { closeContextMenu(); item.run(); });
            }
            container.appendChild(el);
        }
    };
    build(items, menu);
    document.body.appendChild(menu);

    // Keep on screen.
    const rect = menu.getBoundingClientRect();
    menu.style.left = Math.min(x, window.innerWidth - rect.width - 8) + 'px';
    menu.style.top = Math.min(y, window.innerHeight - rect.height - 8) + 'px';

    setTimeout(() => {
        window.addEventListener('mousedown', hideOnOutside, { once: true });
    }, 0);
    function hideOnOutside(ev) {
        if (!menu.contains(ev.target)) closeContextMenu();
        else window.addEventListener('mousedown', hideOnOutside, { once: true });
    }
}

export function closeContextMenu() {
    document.querySelectorAll('#ctx-menu').forEach(el => el.remove());
}

import { t } from '../i18n.js';
function label(key) { return t(key); }
