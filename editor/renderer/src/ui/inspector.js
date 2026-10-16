// Cairn Engine editor - inspector panel.
// Entity properties, components (reflection-driven) and scene settings.
//
// Layout contract, and the reason this file is shaped the way it is: every
// property row is a two-column CSS grid with a fixed label column, so values
// start at the same x on every row of every block. Numbers are monospace and
// tabular (see .num-field in theme.css) so a value cannot shift the column it
// sits in while it is being dragged.
'use strict';

import path from 'path';
import fs from 'fs';

import { editor, engineInstance, selectedEntity, markDirty, pushUndo, scriptClassMeta } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';

// Component type -> i18n key. A `.toLowerCase()` lookup cannot work here:
// MeshRenderer would resolve to 'comp.meshrenderer', which is not a key, and
// the raw key would be rendered as the block title.
const COMP_TITLE = {
    Transform: 'comp.transform',
    Camera: 'comp.camera',
    Light: 'comp.light',
    MeshRenderer: 'comp.meshRenderer',
    SpriteRenderer: 'comp.spriteRenderer',
    TextRenderer: 'comp.textRenderer',
    RigidBody: 'comp.rigidBody',
    BoxCollider: 'comp.boxCollider',
    SphereCollider: 'comp.sphereCollider',
    Collider2D: 'comp.collider2d',
    Script: 'comp.script',
};

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

    refresh() { invalidateAssetList(); this.render(); }

    // Value-only refresh. Rebuilding the panel on every drag step would drop
    // the caret out of any focused field and re-run every control factory, so
    // rows that can be driven externally register a _refresh closure instead.
    renderLight() {
        for (const row of this.bodyEl.querySelectorAll('.prop-row')) {
            if (row._refresh) row._refresh();
        }
    }

    render() {
        const el = this.bodyEl;
        const ent = selectedEntity();
        el.innerHTML = '';

        if (!ent) {
            el.appendChild(emptyLine(t('comp.none')));
            this.renderSceneSettings(el);
            return;
        }

        const header = document.createElement('div');
        header.className = 'insp-entity-header';
        header.innerHTML = `
            <div class="insp-name-row">
                <input type="checkbox" id="insp-active" ${ent.active ? 'checked' : ''} title="${escapeAttr(t('insp.active'))}">
                <input type="text" id="insp-name" value="${escapeHtml(ent.name)}" spellcheck="false" title="${escapeAttr(t('insp.name'))}">
            </div>
            <div class="insp-name-row">
                <label for="insp-tag">${escapeHtml(t('entity.tag'))}</label>
                <input type="text" id="insp-tag" value="${escapeHtml(ent.tag)}" spellcheck="false">
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

        for (const comp of ent.components) el.appendChild(this.componentBlock(ent, comp));

        const addRow = document.createElement('div');
        addRow.className = 'add-component-row';
        const addBtn = document.createElement('button');
        addBtn.className = 'btn';
        addBtn.textContent = t('comp.add');
        addBtn.addEventListener('click', ev => this.addComponentMenu(ev, ent));
        addRow.appendChild(addBtn);
        el.appendChild(addRow);

        this.renderSceneSettings(el);
    }

    renderSceneSettings(el) {
        const eng = engineInstance();
        const s = eng.getSceneSettings();
        const block = section(t('insp.sceneSettings'), 'scene');
        const body = block.querySelector('.comp-body');
        const set = (key, v) => { eng.setSceneSetting(key, v); };

        body.appendChild(vectorRow(t('insp.gravity'), () => s.gravity, v => set('gravity', v), false, 3));
        body.appendChild(colorRow(t('insp.ambient'), () => s.ambient, v => set('ambient', v)));
        body.appendChild(colorRow(t('insp.background'), () => s.background, v => set('background', v)));
        body.appendChild(boolRow(t('insp.fog'), () => s.fogEnabled, v => { set('fogEnabled', v); markDirty(); }));
        body.appendChild(colorRow(t('insp.fogColor'), () => s.fogColor, v => set('fogColor', v)));
        body.appendChild(numberRow(t('insp.fogDensity'), () => s.fogDensity, 0.001, v => set('fogDensity', v)));
        body.appendChild(numberRow(t('insp.exposure'), () => s.exposure, 0.01, v => set('exposure', v)));
        body.appendChild(enumRow(t('insp.tonemap'), () => s.tonemap,
            ['none', 'reinhard', 'aces', 'filmic'].map(k => t('insp.tonemap.' + k)),
            v => { set('tonemap', v); markDirty(); }));

        body.appendChild(boolRow(t('insp.postFxaa'), () => s.postFxaa, v => { set('postFxaa', v); markDirty(); }));
        body.appendChild(numberRow(t('insp.postContrast'), () => s.postContrast, 0.01, v => set('postContrast', v)));
        body.appendChild(numberRow(t('insp.postSaturation'), () => s.postSaturation, 0.01, v => set('postSaturation', v)));
        body.appendChild(numberRow(t('insp.postBloomStrength'), () => s.postBloomStrength, 0.01, v => set('postBloomStrength', v)));
        body.appendChild(numberRow(t('insp.postBloomThreshold'), () => s.postBloomThreshold, 0.01, v => set('postBloomThreshold', v)));
        body.appendChild(numberRow(t('insp.postVignette'), () => s.postVignette, 0.01, v => set('postVignette', v)));
        body.appendChild(numberRow(t('insp.postVignetteRadius'), () => s.postVignetteRadius, 0.01, v => set('postVignetteRadius', v)));
        body.appendChild(numberRow(t('insp.postVignetteSoftness'), () => s.postVignetteSoftness, 0.01, v => set('postVignetteSoftness', v)));
        body.appendChild(numberRow(t('insp.textureMemory'), () => s.textureMemoryMB, 8, v => set('textureMemoryMB', v)));

        // Rasteriser. Auto prefers the GPU and falls back to software, so the
        // same scene file keeps working on a machine with no usable Vulkan.
        body.appendChild(enumRow(t('insp.renderBackend'), () => s.renderBackend,
            ['auto', 'vulkan', 'software'].map(k => t('insp.renderBackend.' + k)),
            v => { set('renderBackend', v); markDirty(); }));
        body.appendChild(enumRow(t('insp.msaa'), () => [1, 2, 4, 8].indexOf(s.msaa),
            [1, 2, 4, 8].map(k => `${k}x`),
            v => { set('msaa', [1, 2, 4, 8][v]); markDirty(); }));
        el.appendChild(block);
    }

    componentBlock(ent, comp) {
        const eng = engineInstance();
        const isTransform = comp.type === 'Transform';
        const collapsed = this.collapsed.has(comp.type);
        const titleKey = COMP_TITLE[comp.type];
        const block = section(titleKey ? t(titleKey) : comp.type, comp.type);

        const head = block.querySelector('.comp-header');
        head.querySelector('.toggle-arrow').textContent = collapsed ? '▸' : '▾';
        const body = block.querySelector('.comp-body');
        if (collapsed) body.style.display = 'none';

        if (!isTransform) {
            const remove = document.createElement('span');
            remove.className = 'remove';
            remove.title = t('insp.removeComponent');
            remove.innerHTML = icon('close');
            remove.addEventListener('click', () => {
                pushUndo();
                eng.removeComponent(ent.id, comp.type);
                markDirty();
                this.render();
            });
            head.appendChild(remove);
        }

        head.addEventListener('click', () => {
            if (this.collapsed.has(comp.type)) this.collapsed.delete(comp.type);
            else this.collapsed.add(comp.type);
            this.render();
        });

        if (isTransform) {
            const read = key => () => ent.transform[key];
            body.appendChild(vectorRow(t('insp.position'), read('position'), v => eng.setComponentProp(ent.id, 'Transform', 'position', v), true, 3));
            body.appendChild(vectorRow(t('insp.rotation'), read('rotation'), v => eng.setComponentProp(ent.id, 'Transform', 'rotation', v), true, 3));
            body.appendChild(vectorRow(t('insp.scale'), read('scale'), v => eng.setComponentProp(ent.id, 'Transform', 'scale', v), true, 3));
        } else if (comp.type === 'Script') {
            this.scriptBody(body, ent, comp);
        } else if (comp.meta) {
            for (const md of comp.meta) {
                const commit = v => {
                    eng.setComponentProp(ent.id, comp.type, md.name, v);
                    markDirty();
                };
                body.appendChild(this.propControl(md, () => comp.props[md.name], commit));
            }
            body.appendChild(boolRow(t('insp.enabled'), () => comp.enabled, v => {
                eng.setComponentProp(ent.id, comp.type, 'enabled', v);
                markDirty();
            }));
        }
        return block;
    }

    scriptBody(body, ent, comp) {
        const eng = engineInstance();
        if (!editor.scriptClasses.length) {
            body.innerHTML = `<div class="form-hint">${escapeHtml(t('comp.script.noClass'))}</div>`;
            if (comp.scriptClass) {
                const row = readOnlyRow(t('insp.class'), comp.scriptClass);
                body.appendChild(row);
            }
            return;
        }
        const select = document.createElement('select');
        select.innerHTML = `<option value=""></option>${editor.scriptClasses.map(c => `
            <option value="${escapeHtml(c.name)}" ${c.name === comp.scriptClass ? 'selected' : ''}>${escapeHtml(c.name)}</option>`).join('')}`;
        select.addEventListener('change', ev => {
            pushUndo();
            eng.setComponentProp(ent.id, 'Script', 'class', ev.target.value);
            markDirty();
            this.render();
        });
        body.appendChild(labelRow(t('comp.script.pickClass'), select));

        const meta = scriptClassMeta(comp.scriptClass);
        if (meta) {
            for (const p of meta.props) {
                const commit = v => {
                    eng.setComponentProp(ent.id, 'Script', p.name, v);
                    markDirty();
                };
                body.appendChild(this.propControl(
                    { name: p.name, label: p.name, kind: p.type },
                    () => comp.props[p.name] === undefined ? p.default : comp.props[p.name],
                    commit));
            }
        }
    }

    propControl(md, read, commit) {
        switch (md.kind) {
            case 'float': return numberRow(md.label, read, null, commit);
            case 'int': return numberRow(md.label, read, 1, commit, true);
            case 'bool': return boolRow(md.label, read, commit);
            case 'string': return textRow(md.label, read, commit);
            case 'vec2': return vectorRow(md.label, read, commit, false, 2);
            case 'vec3': return vectorRow(md.label, read, commit, false, 3);
            case 'vec4': return vectorRow(md.label, read, commit, false, 4);
            case 'color': return colorRow(md.label, read, commit);
            case 'enum': return enumRow(md.label, read, md.options, commit);
            case 'asset': return assetRow(md.label, read, md.assetKind, commit);
            case 'object': return textRow(md.label, read, commit);
            default: return textRow(md.label, read, commit);
        }
    }

    addComponentMenu(ev, ent) {
        import('./contextmenu.js').then(({ showContextMenu }) => {
            const names = ['Camera', 'Light', 'MeshRenderer', 'SpriteRenderer', 'TextRenderer',
                'RigidBody', 'BoxCollider', 'SphereCollider', 'Collider2D', 'Script'];
            showContextMenu(ev.clientX, ev.clientY, names.map(n => ({
                label: t(COMP_TITLE[n] || 'comp.script'),
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
// Blocks and rows.
// ---------------------------------------------------------------------------

function section(title, key) {
    const block = document.createElement('div');
    block.className = 'comp-block';
    block.dataset.block = key;
    block.innerHTML = `
        <div class="comp-header" role="button" tabindex="0">
            <span class="toggle-arrow"></span><span class="comp-title">${escapeHtml(title)}</span>
        </div>
        <div class="comp-body"></div>`;
    // Enter/Space parity with the click that folds the block.
    block.querySelector('.comp-header').addEventListener('keydown', ev => {
        if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); ev.currentTarget.click(); }
    });
    return block;
}

// Labels carry a title unconditionally. The label column is fixed, so a long
// prop name is going to be ellipsised at some panel width; a truncated label
// with no tooltip is a dead end.
function labelEl(text, draggable) {
    const l = document.createElement('label');
    l.textContent = text;
    l.title = text;
    if (!draggable) l.className = 'no-drag';
    return l;
}

function propRow(labelText, field, draggable) {
    const row = document.createElement('div');
    row.className = 'prop-row';
    row.appendChild(labelEl(labelText, draggable));
    row.appendChild(field);
    return row;
}

function labelRow(labelText, field) {
    return propRow(labelText, field, false);
}

function readOnlyRow(labelText, value) {
    const span = document.createElement('span');
    span.className = 'muted';
    span.textContent = value;
    span.title = value;
    return propRow(labelText, span, false);
}

function emptyLine(text) {
    const d = document.createElement('div');
    d.className = 'empty-line';
    d.textContent = text;
    return d;
}

// ---------------------------------------------------------------------------
// Property controls. Every factory takes `read()` rather than a value so the
// row can be refreshed in place, and every one of them fills the value column
// so controls line up down the panel regardless of kind.
// ---------------------------------------------------------------------------

function makeNumField(read, cls, commit, step, integer) {
    const wrap = document.createElement('div');
    wrap.className = 'num-field' + (cls ? ' ' + cls : '');
    wrap.innerHTML = '<span class="prefix"></span><input type="text" spellcheck="false">';
    const input = wrap.querySelector('input');
    const fmt = v => (integer ? String(Math.round(v)) : String(round(v, 4)));
    const paint = () => { input.value = fmt(Number(read()) || 0); };
    paint();
    input.addEventListener('change', () => {
        let v = parseFloat(input.value);
        if (isNaN(v)) { paint(); return; }
        if (integer) v = Math.round(v);
        commit(v);
        paint();
    });
    input.addEventListener('keydown', ev => {
        if (ev.key === 'Enter') input.blur();
        if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown') return;
        ev.preventDefault();
        // Shift is coarse, matching the drag-scrub modifier. Previously the
        // arrow keys treated Shift as the finer step while the scrub treated
        // it as coarser, so the same chord meant opposite things.
        const base = step || 0.1;
        const delta = (ev.key === 'ArrowUp' ? 1 : -1) * base * (ev.shiftKey ? 10 : 1);
        let v = (parseFloat(input.value) || 0) + delta;
        if (integer) v = Math.round(v);
        commit(v);
        paint();
    });
    wrap._paint = paint;
    return wrap;
}

function numberRow(label, read, step, commit, integer) {
    const field = makeNumField(read, '', commit, step, integer);
    const row = propRow(label, field, true);
    row._refresh = () => field._paint();
    dragScrub(row.querySelector('label'), read, commit, step || 0.05);
    return row;
}

function boolRow(label, read, commit) {
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!read();
    input.addEventListener('change', () => commit(input.checked));
    const row = propRow(label, input, false);
    row._refresh = () => { input.checked = !!read(); };
    return row;
}

function textRow(label, read, commit) {
    const input = document.createElement('input');
    input.type = 'text';
    input.spellcheck = false;
    input.value = read() ?? '';
    input.addEventListener('change', () => commit(input.value));
    const row = propRow(label, input, false);
    row._refresh = () => { input.value = read() ?? ''; };
    return row;
}

function vectorRow(label, read, commit, axes, n = 3) {
    const holder = document.createElement('div');
    holder.className = 'prop-vector';
    const fields = [];
    for (let i = 0; i < n; ++i) {
        const f = makeNumField(() => {
            const v = read();
            return Array.isArray(v) ? v[i] : 0;
        }, axes ? ['ax', 'ay', 'az'][i] : '', v => {
            const cur = Array.isArray(read()) ? [...read()] : new Array(n).fill(0);
            cur[i] = v;
            commit(cur);
        });
        fields.push(f);
        holder.appendChild(f);
    }
    const row = propRow(label, holder, true);
    row._refresh = () => fields.forEach(f => f._paint());
    // Scrubbing a vector label drives every axis. That is the documented
    // behaviour; the row repaints because three values just changed.
    dragScrub(row.querySelector('label'), () => {
        const v = read();
        return Array.isArray(v) ? v[0] : 0;
    }, v => commit(new Array(n).fill(v)), 0.05);
    row.querySelector('label').addEventListener('mouseup', () => row._refresh());
    return row;
}

function colorRow(label, read, commit) {
    const holder = document.createElement('div');
    holder.className = 'prop-color';
    const swatch = document.createElement('input');
    swatch.type = 'color';
    const alpha = document.createElement('input');
    alpha.type = 'range';
    alpha.min = 0; alpha.max = 1; alpha.step = 0.01;
    const paint = () => {
        const arr = Array.isArray(read()) ? read() : [1, 1, 1, 1];
        swatch.value = rgbaToHex(arr);
        alpha.value = arr[3] ?? 1;
        alpha.title = t('insp.alpha');
    };
    paint();
    const emitColor = () => {
        const [r, g, b] = hexToRgb(swatch.value);
        commit([r, g, b, parseFloat(alpha.value)]);
        swatch.title = swatch.value;
    };
    swatch.addEventListener('input', emitColor);
    alpha.addEventListener('input', emitColor);
    holder.append(swatch, alpha);
    const row = propRow(label, holder, false);
    row._refresh = paint;
    return row;
}

function enumRow(label, read, options, commit) {
    const select = document.createElement('select');
    select.innerHTML = (options || []).map((o, i) =>
        `<option value="${i}" ${i === read() ? 'selected' : ''}>${escapeHtml(o)}</option>`).join('');
    select.addEventListener('change', ev => commit(parseInt(ev.target.value, 10)));
    const row = propRow(label, select, false);
    row._refresh = () => { select.value = String(read()); };
    return row;
}

function assetRow(label, read, kind, commit) {
    const select = document.createElement('select');
    const exts = kind === 'model' ? ['.obj'] : ['.png', '.jpg', '.jpeg', '.bmp'];
    const assets = listAssets(exts);
    select.innerHTML = `<option value=""></option>${assets.map(a =>
        `<option value="${escapeHtml(a)}" ${a === read() ? 'selected' : ''}>${escapeHtml(a)}</option>`).join('')}`;
    select.addEventListener('change', () => commit(select.value));
    return propRow(label, select, false);
}

// Walks <project>/Assets on every asset-typed property, on every render.
// Cached per inspector lifetime and invalidated by refresh(), which every
// structural change in the app routes through.
let assetListCache = null;
function listAssets(exts) {
    const root = editor.project?.path;
    if (!root) return [];
    const key = root + '|' + exts.join(',');
    if (assetListCache && assetListCache.key === key) return assetListCache.list;
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
    const list = out.sort();
    assetListCache = { key, list };
    return list;
}
export function invalidateAssetList() { assetListCache = null; }

// Drag-to-scrub on a label. The start value is read at mousedown, not at
// render: capturing it at render time meant a second drag on the same label
// jumped, because nothing re-rendered in between to update the capture.
function dragScrub(labelEl, read, commit, step) {
    if (!labelEl) return;
    labelEl.addEventListener('mousedown', ev => {
        if (ev.button !== 0) return;
        ev.preventDefault();
        const startX = ev.clientX;
        const start = Number(read()) || 0;
        // Hold the scrub cursor for the whole gesture so the affordance does
        // not flicker when the pointer outruns the 88px label.
        document.body.style.cursor = 'ew-resize';
        const move = me => commit(start + (me.clientX - startX) * step * (me.shiftKey ? 10 : 1));
        const up = () => {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
            document.body.style.cursor = '';
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
function escapeAttr(s) { return escapeHtml(s); }