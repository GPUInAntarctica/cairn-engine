// Cairn Engine editor - hierarchy panel (world outliner).
'use strict';

import { editor, entityList, selectEntity, destroyEntity, duplicateEntity, createEntity, engineInstance } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';
import { deleteConfirm } from './dialogs.js';

const KIND_ICONS = {
    'Camera': 'camera',
    'Light': 'light',
    'SpriteRenderer': 'sprite',
    'TextRenderer': 'text',
    'Script': 'script',
};

export class Hierarchy {
    constructor(bodyEl) {
        this.bodyEl = bodyEl;
        this.filter = '';
        this.expanded = new Set();
        bodyEl.innerHTML = `
            <div class="tree-search"><input type="text" id="hier-search" placeholder="${t('hierarchy.search')}" spellcheck="false"></div>
            <div class="panel-body" id="hier-tree" style="top:auto; position:static; overflow-y:auto;"></div>`;

        bodyEl.querySelector('#hier-search').addEventListener('input', ev => {
            this.filter = ev.target.value.toLowerCase();
            this.renderTree();
        });
        bodyEl.querySelector('#hier-tree').addEventListener('contextmenu', ev => {
            if (ev.target.closest('.tree-row')) return;
            ev.preventDefault();
            this.contextMenu(ev, null);
        });

        on('scene-changed', () => this.renderTree());
        on('selection-changed', () => this.renderTree());
        on('entity-edited', () => { /* light updates only; full refresh on structural ops */ });
        on('project-opened', () => { this.expanded.clear(); this.renderTree(); });
        this.renderTree();
    }

    refresh() { this.renderTree(); }

    renderTree() {
        const tree = this.bodyEl.querySelector('#hier-tree');
        if (!tree) return;
        const entities = entityList();
        const byParent = new Map();
        for (const e of entities) {
            const key = e.parent || 0;
            if (!byParent.has(key)) byParent.set(key, []);
            byParent.get(key).push(e);
        }
        tree.innerHTML = '';
        const roots = byParent.get(0) || [];
        if (!roots.length) {
            tree.innerHTML = `<div class="muted" style="padding:14px 12px; font-size:11.5px;">${t('hierarchy.empty')}</div>`;
            return;
        }
        const matches = e => !this.filter || e.name.toLowerCase().includes(this.filter);
        const build = (list, depth) => {
            for (const e of list) {
                const children = byParent.get(e.id) || [];
                const show = matches(e) || this.filter;
                if (show) {
                    const row = document.createElement('div');
                    row.className = 'tree-row' + (e.id === editor.selection ? ' selected' : '');
                    const hasChildren = children.length > 0;
                    const expanded = this.expanded.has(e.id) || this.filter;
                    const iconName = componentName(e, KIND_ICONS);
                    row.style.paddingLeft = (8 + depth * 14) + 'px';
                    row.innerHTML = `
                        <span class="toggle">${hasChildren ? (expanded ? '▾' : '▸') : ''}</span>
                        <span class="icon">${icon(iconName, 12)}</span>
                        <span class="label ${e.active ? '' : 'inactive-mark'}">${escapeHtml(e.name)}</span>`;
                    row.addEventListener('click', () => selectEntity(e.id));
                    row.addEventListener('dblclick', () => this.renameInline(row, e));
                    row.addEventListener('contextmenu', ev => {
                        ev.preventDefault();
                        selectEntity(e.id);
                        this.contextMenu(ev, e);
                    });
                    if (hasChildren) {
                        row.querySelector('.toggle').addEventListener('click', ev => {
                            ev.stopPropagation();
                            if (this.expanded.has(e.id)) this.expanded.delete(e.id);
                            else this.expanded.add(e.id);
                            this.renderTree();
                        });
                    }
                    tree.appendChild(row);
                }
                if (children.length && (this.expanded.has(e.id) || this.filter)) build(children, depth + 1);
            }
        };
        build(roots, 0);
    }

    renameInline(row, e) {
        const label = row.querySelector('.label');
        const input = document.createElement('input');
        input.type = 'text';
        input.value = e.name;
        input.style.cssText = 'flex:1; height:18px; font-size:11.5px;';
        label.replaceWith(input);
        input.focus();
        input.select();
        const commit = () => {
            engineInstance().setEntityName(e.id, input.value.trim() || e.name);
            emit('scene-changed');
        };
        input.addEventListener('keydown', ev => {
            if (ev.key === 'Enter') commit();
            if (ev.key === 'Escape') this.renderTree();
        });
        input.addEventListener('blur', commit);
    }

    contextMenu(ev, entity) {
        const { showContextMenu } = require('./contextmenu.js');
        const items = [];
        if (entity) {
            items.push(
                { label: 'ctx.duplicate', run: () => duplicateEntity(entity.id) },
                { label: 'ctx.rename', run: () => {
                    const row = [...this.bodyEl.querySelectorAll('.tree-row')].find(r => r.classList.contains('selected'));
                    if (row) this.renameInline(row, entity);
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
    }
}

function componentName(e, map) {
    for (const c of e.components) {
        if (c.type === 'Camera') return 'camera';
        if (c.type === 'Light') return 'light';
        if (c.type === 'SpriteRenderer') return 'sprite';
        if (c.type === 'MeshRenderer') return 'cube';
    }
    return 'cube';
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
