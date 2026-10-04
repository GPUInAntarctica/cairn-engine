// Cairn Engine editor - inspector panel.
// Entity properties, components (reflection-driven) and scene settings.
'use strict';

import path from 'path';
import fs from 'fs';

import { editor, engineInstance, entityList, selectedEntity, markDirty, pushUndo, scriptClassMeta } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';

export class Inspector {
    constructor(bodyEl) {
        this.bodyEl = bodyEl;
        this.collapsed = new Set();
        on('selection-changed', () => this.render());
        on('scene-changed', () => this.render());
        on('entity-edited', () => this.renderLight());
        on('script-classes-changed', () => this.render());
        this.render();
    }

    refresh() { this.render(); }

    renderLight() {
        // Re-read values without rebuilding DOM (keeps focus in text fields).
        const ent = selectedEntity();
        if (!ent) return;
        this.bodyEl.querySelectorAll('[data-prop]').forEach(input => {
            const [compType, prop] = input.dataset.prop.split('|');
            if (compType === 'Transform') return;
        });
    }

    render() {
        const el = this.bodyEl;
        const ent = selectedEntity();
        el.innerHTML = '';

        if (!ent) {
            el.innerHTML = `<div class="muted" style="padding:14px 12px; font-size:11.5px;">${t('comp.none')}</div>`;
            this.renderSceneSettings(el);
            return;
        }

        // Entity header.
        const header = document.createElement('div');
        header.className = 'insp-entity-header';
        header.innerHTML = `
            <div class="insp-name-row">
                <input type="checkbox" id="insp-active" ${ent.active ? 'checked' : ''} title="Active">
                <input type="text" id="insp-name" value="${escapeHtml(ent.name)}" spellcheck="false">
            </div>
            <div class="insp-name-row">
                <label class="muted" style="font-size:11px;">${t('entity.tag')}</label>
                <input type="text" id="insp-tag" value="${escapeHtml(ent.tag)}" spellcheck="false" placeholder="">
            </div>`;
        el.appendChild(header);

        header.querySelector('#insp-name').addEventListener('change', ev => {
            engineInstance().setEntityName(ent.id, ev.target.value.trim() || ent.name);
            emit('scene-changed');
        });
        header.querySelector('#insp-tag').addEventListener('change', ev => {
            engineInstance().setEntityTag(ent.id, ev.target.value.trim());
            markDirty();
        });
        header.querySelector('#insp-active').addEventListener('change', ev => {
            engineInstance().setEntityActive(ent.id, ev.target.checked);
            markDirty();
            emit('scene-changed');
        });

        // Components.
        for (const comp of ent.components) {
            el.appendChild(this.componentBlock(ent, comp));
        }

        // Add component.
        const addRow = document.createElement('div');
        addRow.className = 'add-component-row';
        addRow.innerHTML = `<button class="btn" style="width:100%;">${t('comp.add')}</button>`;
        addRow.querySelector('button').addEventListener('click', ev => {
            this.addComponentMenu(ev, ent);
        });
        el.appendChild(addRow);

        this.renderSceneSettings(el);
    }

    renderSceneSettings(el) {
        const eng = engineInstance();
        const s = eng.getSceneSettings();
        const block = document.createElement('div');
        block.className = 'comp-block';
        block.innerHTML = `
            <div class="comp-header"><span class="toggle-arrow">▾</span>${t('insp.sceneSettings')}</div>
            <div class="comp-body"></div>`;
        const body = block.querySelector('.comp-body');
        body.appendChild(vectorRow(t('insp.gravity'), 'gravity', s.gravity, v => {
            eng.setSceneSetting('gravity', v);
        }, false));
        body.appendChild(colorRow(t('insp.ambient'), s.ambient, v => eng.setSceneSetting('ambient', v)));
        body.appendChild(colorRow(t('insp.background'), s.background, v => eng.setSceneSetting('background', v)));
        const fogRow = document.createElement('div');
        fogRow.className = 'prop-row';
        fogRow.innerHTML = `<label class="no-drag">${t('insp.fog')}</label><input type="checkbox" ${s.fogEnabled ? 'checked' : ''}>`;
        fogRow.querySelector('input').addEventListener('change', ev => {
            eng.setSceneSetting('fogEnabled', ev.target.checked);
            markDirty();
        });
        body.appendChild(fogRow);
        body.appendChild(colorRow(t('insp.fogColor'), s.fogColor, v => eng.setSceneSetting('fogColor', v)));
        body.appendChild(numberRow(t('insp.fogDensity'), s.fogDensity, 0.001, v => eng.setSceneSetting('fogDensity', v)));
        el.appendChild(block);
    }

    componentBlock(ent, comp) {
        const eng = engineInstance();
        const block = document.createElement('div');
        block.className = 'comp-block';
        const isTransform = comp.type === 'Transform';
        const collapsed = this.collapsed.has(comp.type);
        const title = isTransform ? t('comp.transform') : comp.type === 'Script' ? t('comp.script') : t('comp.' + comp.type.toLowerCase());

        block.innerHTML = `
            <div class="comp-header">
                <span class="toggle-arrow">${collapsed ? '▸' : '▾'}</span>
                <span>${title}</span>
                ${!isTransform ? `<span class="remove" title="Remove">${icon('close', 11)}</span>` : ''}
            </div>
            <div class="comp-body" style="${collapsed ? 'display:none;' : ''}"></div>`;

        block.querySelector('.comp-header').addEventListener('click', ev => {
            if (ev.target.closest('.remove')) return;
            if (collapsed) this.collapsed.delete(comp.type); else this.collapsed.add(comp.type);
            this.render();
        });
        const removeBtn = block.querySelector('.remove');
        if (removeBtn) removeBtn.addEventListener('click', () => {
            pushUndo();
            eng.removeComponent(ent.id, comp.type);
            markDirty();
            this.render();
        });

        const body = block.querySelector('.comp-body');

        if (isTransform) {
            body.appendChild(vectorRow('Position', 'position', ent.transform.position,
                v => eng.setComponentProp(ent.id, 'Transform', 'position', v), true));
            body.appendChild(vectorRow('Rotation', 'rotation', ent.transform.rotation,
                v => eng.setComponentProp(ent.id, 'Transform', 'rotation', v), true));
            body.appendChild(vectorRow('Scale', 'scale', ent.transform.scale,
                v => eng.setComponentProp(ent.id, 'Transform', 'scale', v), true));
        } else if (comp.type === 'Script') {
            this.scriptBody(body, ent, comp);
        } else if (comp.meta) {
            for (const md of comp.meta) {
                const value = comp.props[md.name];
                const commit = v => {
                    eng.setComponentProp(ent.id, comp.type, md.name, v);
                    markDirty();
                };
                body.appendChild(this.propControl(md, value, commit));
            }
            const enabledRow = document.createElement('div');
            enabledRow.className = 'prop-row';
            enabledRow.innerHTML = `<label class="no-drag">Enabled</label><input type="checkbox" ${comp.enabled ? 'checked' : ''}>`;
            enabledRow.querySelector('input').addEventListener('change', ev => {
                eng.setComponentProp(ent.id, comp.type, 'enabled', ev.target.checked);
                markDirty();
            });
            body.appendChild(enabledRow);
        }
        return block;
    }

    scriptBody(body, ent, comp) {
        const eng = engineInstance();
        if (!editor.scriptClasses.length) {
            body.innerHTML = `<div class="form-hint">${t('comp.script.noClass')}</div>`;
            if (comp.scriptClass) {
                body.insertAdjacentHTML('beforeend', `<div class="prop-row"><label class="no-drag">Class</label><span class="muted">${escapeHtml(comp.scriptClass)}</span></div>`);
            }
            return;
        }
        const row = document.createElement('div');
        row.className = 'prop-row';
        row.innerHTML = `<label class="no-drag">${t('comp.script.pickClass')}</label>
            <select><option value="">—</option>${editor.scriptClasses.map(c => `
                <option value="${escapeHtml(c.name)}" ${c.name === comp.scriptClass ? 'selected' : ''}>${escapeHtml(c.name)}</option>`).join('')}
            </select>`;
        row.querySelector('select').addEventListener('change', ev => {
            pushUndo();
            eng.setComponentProp(ent.id, 'Script', 'class', ev.target.value);
            markDirty();
            this.render();
        });
        body.appendChild(row);

        const meta = scriptClassMeta(comp.scriptClass);
        if (meta) {
            for (const p of meta.props) {
                const value = comp.props[p.name];
                const commit = v => {
                    eng.setComponentProp(ent.id, 'Script', p.name, v);
                    markDirty();
                };
                body.appendChild(this.propControl(
                    { name: p.name, label: p.name, kind: p.type },
                    value === undefined ? p.default : value, commit));
            }
        }
    }

    propControl(md, value, commit) {
        switch (md.kind) {
            case 'float': return numberRow(md.label, value, null, commit);
            case 'int': return numberRow(md.label, value, 1, commit, true);
            case 'bool': return boolRow(md.label, value, commit);
            case 'string': return textRow(md.label, value, commit);
            case 'vec2': return vectorRow(md.label, md.name, value, commit, false, 2);
            case 'vec3': return vectorRow(md.label, md.name, value, commit, false, 3);
            case 'color': return colorRow(md.label, value, commit);
            case 'enum': return enumRow(md.label, value, md.options, commit);
            case 'asset': return assetRow(md.label, value, md.assetKind, commit);
            default: return textRow(md.label, value, commit);
        }
    }

    addComponentMenu(ev, ent) {
        import('./contextmenu.js').then(({ showContextMenu }) => {
            const names = ['Camera', 'Light', 'MeshRenderer', 'SpriteRenderer', 'TextRenderer',
                'RigidBody', 'BoxCollider', 'SphereCollider', 'Collider2D', 'Script'];
            showContextMenu(ev.clientX, ev.clientY, names.map(n => ({
                label: 'comp.' + n.toLowerCase(),
                run: () => {
                    pushUndo();
                    engineInstance().addComponent(ent.id, n);
                    markDirty();
                    this.render();
                },
            })));
        });
    }
}

// ---------------------------------------------------------------------------
// Property controls.
// ---------------------------------------------------------------------------
function makeNumField(value, cls, onCommit, step, integer) {
    const wrap = document.createElement('div');
    wrap.className = 'num-field ' + (cls || '');
    wrap.innerHTML = `<span class="prefix"></span><input type="text" spellcheck="false">`;
    const input = wrap.querySelector('input');
    const fmt = v => (integer ? String(Math.round(v)) : String(round(v, 4)));
    input.value = fmt(value ?? 0);
    input.addEventListener('change', () => {
        let v = parseFloat(input.value);
        if (isNaN(v)) { input.value = fmt(value ?? 0); return; }
        if (integer) v = Math.round(v);
        onCommit(v);
        input.value = fmt(v);
    });
    input.addEventListener('keydown', ev => {
        if (ev.key === 'Enter') input.blur();
        if (ev.key === 'ArrowUp' || ev.key === 'ArrowDown') {
            ev.preventDefault();
            const delta = (ev.key === 'ArrowUp' ? 1 : -1) * (step || (ev.shiftKey ? 0.5 : 0.1));
            let v = parseFloat(input.value) || 0;
            v += delta;
            if (integer) v = Math.round(v);
            onCommit(v);
            input.value = fmt(v);
        }
    });
    return wrap;
}

function numberRow(label, value, step, commit, integer) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label>${escapeHtml(label)}</label>`;
    const field = makeNumField(value, '', commit, step, integer);
    field.style.gridColumn = '2';
    row.appendChild(field);
    dragScrub(row.querySelector('label'), value, commit, step || 0.05);
    return row;
}

function boolRow(label, value, commit) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label class="no-drag">${escapeHtml(label)}</label><input type="checkbox" ${value ? 'checked' : ''}>`;
    row.querySelector('input').addEventListener('change', ev => commit(ev.target.checked));
    return row;
}

function textRow(label, value, commit) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label class="no-drag">${escapeHtml(label)}</label>`;
    const input = document.createElement('input');
    input.type = 'text';
    input.spellcheck = false;
    input.value = value ?? '';
    input.addEventListener('change', () => commit(input.value));
    row.appendChild(input);
    return row;
}

function vectorRow(label, key, value, commit, axes, n = 3) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label>${escapeHtml(label)}</label>`;
    const holder = document.createElement('div');
    holder.className = 'prop-vector';
    holder.style.gridColumn = '2';
    const vals = Array.isArray(value) ? value : [0, 0, 0];
    const classes = axes ? ['ax', 'ay', 'az'] : ['', '', ''];
    const inputs = [];
    for (let i = 0; i < n; ++i) {
        const field = makeNumField(vals[i] || 0, classes[i], v => {
            const out = [...vals];
            out[i] = v;
            commit(out);
        });
        inputs.push(field);
        holder.appendChild(field);
    }
    row.appendChild(holder);
    dragScrub(row.querySelector('label'), vals[0], v => {
        const out = [...vals];
        out.fill(v);
        commit(out);
    }, 0.05);
    return row;
}

function colorRow(label, value, commit) {
    const arr = Array.isArray(value) ? value : [1, 1, 1, 1];
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label class="no-drag">${escapeHtml(label)}</label>`;
    const holder = document.createElement('div');
    holder.style.cssText = 'grid-column:2; display:flex; gap:6px; align-items:center;';
    const rgb = document.createElement('input');
    rgb.type = 'color';
    rgb.value = rgbaToHex(arr);
    const alpha = document.createElement('input');
    alpha.type = 'range';
    alpha.min = 0; alpha.max = 1; alpha.step = 0.01;
    alpha.value = arr[3] ?? 1;
    alpha.style.flex = '1';
    const emitColor = () => {
        const [r, g, b] = hexToRgb(rgb.value);
        commit([r, g, b, parseFloat(alpha.value)]);
    };
    rgb.addEventListener('input', emitColor);
    alpha.addEventListener('input', emitColor);
    holder.append(rgb, alpha);
    row.appendChild(holder);
    return row;
}

function enumRow(label, value, options, commit) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label class="no-drag">${escapeHtml(label)}</label>
        <select>${(options || []).map((o, i) => `<option value="${i}" ${i === value ? 'selected' : ''}>${escapeHtml(o)}</option>`).join('')}</select>`;
    row.querySelector('select').addEventListener('change', ev => commit(parseInt(ev.target.value)));
    return row;
}

function assetRow(label, value, kind, commit) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.innerHTML = `<label class="no-drag">${escapeHtml(label)}</label>`;
    const holder = document.createElement('div');
    holder.style.cssText = 'grid-column:2; display:flex; gap:6px; min-width:0;';
    const select = document.createElement('select');
    select.style.flex = '1';
    const exts = kind === 'model' ? ['.obj'] : ['.png', '.jpg', '.jpeg', '.bmp'];
    const assets = listAssets(exts);
    select.innerHTML = `<option value="">—</option>${assets.map(a =>
        `<option value="${escapeHtml(a)}" ${a === value ? 'selected' : ''}>${escapeHtml(a)}</option>`).join('')}`;
    select.addEventListener('change', () => commit(select.value));
    holder.appendChild(select);
    row.appendChild(holder);
    return row;
}

function listAssets(exts) {
    const root = editor.project?.path;
    if (!root) return [];
    const out = [];
    const walk = (dir, rel) => {
        let entries;
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
        for (const e of entries) {
            const full = path.join(dir, e.name);
            const relPath = rel ? rel + '/' + e.name : e.name;
            if (e.isDirectory()) {
                if (e.name !== 'Library') walk(full, relPath);
            } else if (exts.some(ext => e.name.toLowerCase().endsWith(ext))) {
                out.push(relPath);
            }
        }
    };
    walk(path.join(root, 'Assets'), '');
    return out.sort();
}

// Drag-to-scrub on a label (Unreal-style).
function dragScrub(labelEl, startValue, commit, step) {
    if (!labelEl) return;
    labelEl.addEventListener('mousedown', ev => {
        ev.preventDefault();
        const startX = ev.clientX;
        const start = typeof startValue === 'number' ? startValue : 0;
        const move = me => {
            const delta = (me.clientX - startX) * step * (me.shiftKey ? 10 : 1);
            commit(start + delta);
        };
        const up = () => {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
        };
        window.addEventListener('mousemove', move);
        window.addEventListener('mouseup', up);
    });
}

function rgbaToHex(arr) {
    const [r, g, b] = arr.map(v => Math.round(clamp01(v) * 255));
    return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}
function hexToRgb(hex) {
    const v = parseInt(hex.slice(1), 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255];
}
function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
function round(v, d) { const p = Math.pow(10, d); return Math.round(v * p) / p; }
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
