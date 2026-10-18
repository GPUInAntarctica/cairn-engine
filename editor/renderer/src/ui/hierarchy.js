// Cairn Engine editor - hierarchy panel (world outliner).
//
// Two things matter for performance here, because this is the one panel that
// can hold hundreds of rows:
//   * selection changes repaint two rows, they do not rebuild the tree;
//   * one delegated listener set handles every row, so a rebuild costs N DOM
//     nodes and zero listener registrations.
'use strict';

import { editor, entityList, selectEntity, destroyEntity, duplicateEntity, createEntity, engineInstance } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';
import { deleteConfirm } from './dialogs.js';

const KIND_ICONS = {
    Camera: 'camera',
    Light: 'light',
    SpriteRenderer: 'sprite',
    SpriteAnimator: 'sprite',
    TextRenderer: 'text',
    Script: 'script',
    MeshRenderer: 'cube',
};

export class Hierarchy {
    constructor(bodyEl) {
        this.bodyEl = bodyEl;
        this.filter = '';
        this.expanded = new Set();
        this.rows = [];

        bodyEl.innerHTML = `
            <div class="tree-search"><input type="text" id="hier-search" placeholder="${escapeAttr(t('hierarchy.search'))}" spellcheck="false"></div>
            <div class="panel-body tree" id="hier-tree"></div>`;
        this.tree = bodyEl.querySelector('#hier-tree');
        this.search = bodyEl.querySelector('#hier-search');

        this.search.addEventListener('input', ev => {
            this.filter = ev.target.value.toLowerCase();
            this.renderTree();
        });

        // ---- delegated events: bound once, never per row ---------------------
        this.tree.addEventListener('click', ev => {
            const row = ev.target.closest('.tree-row');
            if (!row) return;
            if (ev.target.closest('.toggle')) {
                ev.stopPropagation();
                const id = Number(row.dataset.id);
                if (this.expanded.has(id)) this.expanded.delete(id);
                else this.expanded.add(id);
                this.renderTree();
                return;
            }
            selectEntity(Number(row.dataset.id));
        });

        this.tree.addEventListener('dblclick', ev => {
            const row = ev.target.closest('.tree-row');
            if (row && !ev.target.closest('.toggle')) this.renameInline(row);
        });

        this.tree.addEventListener('contextmenu', ev => {
            const row = ev.target.closest('.tree-row');
            ev.preventDefault();
            if (row) {
                const id = Number(row.dataset.id);
                if (editor.selection !== id) selectEntity(id);
                this.contextMenu(ev, this.entityById(id));
            } else {
                this.contextMenu(ev, null);
            }
        });

        // Keyboard parity with the mouse: the tree is the primary list in the
        // editor and has to be reachable without a pointer.
        this.tree.addEventListener('keydown', ev => this.onKeyDown(ev));

        on('scene-changed', () => this.renderTree());
        on('selection-changed', () => this.paintSelection());
        on('project-opened', () => { this.expanded.clear(); this.renderTree(); });
        this.renderTree();
    }

    refresh() { this.renderTree(); }

    entityById(id) { return entityList().find(e => e.id === id) || null; }

    // Selection is a class swap on the previously and currently selected rows.
    // Rebuilding 300 rows to change one class was the single largest
    // avoidable cost in the panel.
    paintSelection() {
        let focused = null;
        for (const row of this.rows) {
            const on = Number(row.dataset.id) === editor.selection;
            row.classList.toggle('selected', on);
            row.setAttribute('aria-selected', on ? 'true' : 'false');
            if (on) focused = row;
        }
        // Follow selection with the keyboard cursor, but never yank focus away
        // from a field the user is typing into.
        if (focused && this.tree.contains(document.activeElement) &&
            document.activeElement !== this.search) {
            focused.focus({ preventScroll: false });
        }
    }

    onKeyDown(ev) {
        const rows = this.rows;
        if (!rows.length) return;
        const cur = ev.target.closest('.tree-row');
        const idx = cur ? rows.indexOf(cur) : -1;
        let next = -1;
        if (ev.key === 'ArrowDown') next = Math.min(rows.length - 1, idx + 1);
        else if (ev.key === 'ArrowUp') next = Math.max(0, idx <= 0 ? 0 : idx - 1);
        else if (ev.key === 'Home') next = 0;
        else if (ev.key === 'End') next = rows.length - 1;
        else if (ev.key === 'ArrowRight' && cur) {
            const id = Number(cur.dataset.id);
            if (this.hasChildren(id) && !this.expanded.has(id)) {
                this.expanded.add(id);
                this.renderTree();
                ev.preventDefault();
            }
            return;
        } else if (ev.key === 'ArrowLeft' && cur) {
            const id = Number(cur.dataset.id);
            if (this.expanded.has(id)) {
                this.expanded.delete(id);
                this.renderTree();
            }
            ev.preventDefault();
            return;
        } else if (ev.key === 'Enter' && cur) {
            selectEntity(Number(cur.dataset.id));
            ev.preventDefault();
            return;
        } else if (ev.key === 'F2' && cur) {
            this.renameInline(cur);
            ev.preventDefault();
            return;
        } else {
            return;
        }
        ev.preventDefault();
        if (next >= 0) selectEntity(Number(rows[next].dataset.id));
    }

    hasChildren(id) {
        return entityList().some(e => e.parent === id);
    }

    renderTree() {
        const tree = this.tree;
        const entities = entityList();
        const byParent = new Map();
        for (const e of entities) {
            const key = e.parent || 0;
            if (!byParent.has(key)) byParent.set(key, []);
            byParent.get(key).push(e);
        }

        const roots = byParent.get(0) || [];
        if (!roots.length) {
            tree.innerHTML = `<div class="empty-line">${escapeHtml(t('hierarchy.empty'))}</div>`;
            this.rows = [];
            return;
        }

        // Build into a fragment: the tree is inserted in one operation, so the
        // browser lays out once instead of once per append.
        const frag = document.createDocumentFragment();
        const rows = [];
        const matches = e => !this.filter || e.name.toLowerCase().includes(this.filter);

        const build = (list, depth) => {
            for (const e of list) {
                const children = byParent.get(e.id) || [];
                if (matches(e) || this.filter) {
                    const row = document.createElement('div');
                    row.className = 'tree-row';
                    row.dataset.id = String(e.id);
                    row.dataset.depth = String(depth);
                    row.tabIndex = -1;
                    row.setAttribute('role', 'treeitem');
                    const selected = e.id === editor.selection;
                    row.classList.toggle('selected', selected);
                    row.setAttribute('aria-selected', selected ? 'true' : 'false');
                    const hasChildren = children.length > 0;
                    const expanded = this.expanded.has(e.id) || !!this.filter;
                    const iconName = componentName(e) || 'cube';
                    row.innerHTML = `
                        <span class="toggle">${hasChildren ? (expanded ? '▾' : '▸') : ''}</span>
                        <span class="icon">${icon(iconName)}</span>
                        <span class="label${e.active ? '' : ' inactive-mark'}" title="${escapeAttr(e.name)}">${escapeHtml(e.name)}</span>`;
                    frag.appendChild(row);
                    rows.push(row);
                }
                if (children.length && (this.expanded.has(e.id) || this.filter)) build(children, depth + 1);
            }
        };
        build(roots, 0);

        tree.innerHTML = '';
        tree.appendChild(frag);
        this.rows = rows;
    }

    renameInline(row) {
        const ent = this.entityById(Number(row.dataset.id));
        if (!ent) return;
        const label = row.querySelector('.label');
        if (!label) return;
        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'tree-rename';
        input.value = ent.name;
        input.spellcheck = false;
        label.replaceWith(input);
        input.focus();
        input.select();
        let done = false;
        const finish = commit => {
            if (done) return;
            done = true;
            if (commit) {
                const next = input.value.trim();
                if (next && next !== ent.name) {
                    engineInstance().setEntityName(ent.id, next);
                    emit('scene-changed');
                    return;
                }
            }
            this.renderTree();
        };
        input.addEventListener('keydown', ev => {
            if (ev.key === 'Enter') { ev.preventDefault(); finish(true); }
            else if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
        });
        input.addEventListener('blur', () => finish(true));
    }

    contextMenu(ev, entity) {
        import('./contextmenu.js').then(({ showContextMenu }) => {
            const items = [];
            if (entity) {
                items.push(
                    { label: 'ctx.duplicate', run: () => duplicateEntity(entity.id) },
                    { label: 'ctx.rename', run: () => {
                        const row = this.rows.find(r => r.classList.contains('selected'));
                        if (row) this.renameInline(row);
                    } },
                    { label: 'ctx.delete', run: async () => {
                        if (await deleteConfirm('entity', entity.name)) destroyEntity(entity.id);
                    } },
                    '-',
                );
            }
            items.push({
                label: 'ctx.create',
                children: [
                    { label: 'ctx.create.empty', run: () => createEntity('empty') },
                    '-',
                    { label: 'ctx.create.cube', run: () => createEntity('cube') },
                    { label: 'ctx.create.sphere', run: () => createEntity('sphere') },
                    { label: 'ctx.create.plane', run: () => createEntity('plane') },
                    { label: 'ctx.create.cylinder', run: () => createEntity('cylinder') },
                    { label: 'ctx.create.cone', run: () => createEntity('cone') },
                    { label: 'ctx.create.torus', run: () => createEntity('torus') },
                    { label: 'ctx.create.sprite', run: () => createEntity('sprite') },
                    '-',
                    { label: 'ctx.create.camera', run: () => createEntity('camera') },
                    { label: 'ctx.create.light.directional', run: () => createEntity('light-directional') },
                    { label: 'ctx.create.light.point', run: () => createEntity('light-point') },
                ],
            });
            showContextMenu(ev.clientX, ev.clientY, items);
        });
    }
}

function componentName(e) {
    for (const c of e.components || []) {
        const mapped = KIND_ICONS[c.type];
        if (mapped) return mapped;
    }
    return null;
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
function escapeAttr(s) { return escapeHtml(s); }