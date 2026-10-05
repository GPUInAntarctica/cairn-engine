// Cairn Engine editor - modal dialogs.
//
// One dialog frame: title bar, body, action row. Escape and a backdrop click
// both resolve to null; Tab is trapped inside the frame so a dialog can never
// leave focus stranded behind the scrim.
'use strict';

import fs from 'fs';

import { t } from '../i18n.js';
import { ipc } from '../ipc.js';
import { icon } from '../icons.js';

const FOCUSABLE = 'button, input, select, textarea, [tabindex]:not([tabindex="-1"])';

export function openDialog(title, buildBody) {
    const backdrop = document.createElement('div');
    backdrop.className = 'dialog-backdrop';
    const dlg = document.createElement('div');
    dlg.className = 'dialog';
    dlg.setAttribute('role', 'dialog');
    dlg.setAttribute('aria-modal', 'true');
    dlg.setAttribute('aria-label', title);
    dlg.innerHTML = `
        <div class="dialog-title"><span>${escapeHtml(title)}</span><span class="close" role="button" aria-label="${escapeAttr(t('dialog.close'))}" tabindex="0">${icon('close')}</span></div>
        <div class="dialog-body"></div>
        <div class="dialog-actions"></div>`;
    backdrop.appendChild(dlg);
    document.body.appendChild(backdrop);

    let resolver = null;
    const promise = new Promise(resolve => { resolver = resolve; });
    let done = false;
    const close = value => {
        if (done) return;
        done = true;
        window.removeEventListener('keydown', onKey, true);
        backdrop.remove();
        if (resolver) resolver(value);
    };

    const closeBtn = dlg.querySelector('.close');
    closeBtn.addEventListener('click', () => close(null));
    closeBtn.addEventListener('keydown', ev => {
        if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); close(null); }
    });
    backdrop.addEventListener('mousedown', ev => { if (ev.target === backdrop) close(null); });

    function onKey(ev) {
        if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); close(null); return; }
        if (ev.key !== 'Tab') return;
        const items = [...dlg.querySelectorAll(FOCUSABLE)].filter(el => !el.disabled && el.offsetParent !== null);
        if (!items.length) return;
        const first = items[0], last = items[items.length - 1];
        if (ev.shiftKey && document.activeElement === first) { ev.preventDefault(); last.focus(); }
        else if (!ev.shiftKey && document.activeElement === last) { ev.preventDefault(); first.focus(); }
        else if (!dlg.contains(document.activeElement)) { ev.preventDefault(); first.focus(); }
    }
    window.addEventListener('keydown', onKey, true);

    const actions = dlg.querySelector('.dialog-actions');
    const body = dlg.querySelector('.dialog-body');
    buildBody(body, close, actions);

    // Prefer the first field; otherwise the primary action. Focusing the
    // primary action means a keyboard user can submit with Enter or Space
    // without hunting for the default button first.
    if (!body.contains(document.activeElement)) {
        const target = body.querySelector('input, select, textarea') ||
            actions.querySelector('.btn.primary') || actions.querySelector('.btn');
        target?.focus();
    }
    return promise;
}

export function confirmDialog(messageKey, buttons, vars) {
    return openDialog(t('dialog.confirm'), (body, close, actions) => {
        const msg = document.createElement('div');
        msg.className = 'dialog-message';
        msg.textContent = t(messageKey, vars);
        body.appendChild(msg);
        for (const b of buttons) {
            const btn = document.createElement('button');
            btn.className = 'btn' + (b.primary ? ' primary' : '');
            btn.textContent = t(b.label);
            btn.addEventListener('click', () => close(b.value));
            actions.appendChild(btn);
        }
    });
}

export async function deleteConfirm(kind, name) {
    const key = kind === 'entity' ? 'dialog.delete.entity' : 'dialog.delete.asset';
    const choice = await confirmDialog(key, [
        { label: 'common.delete', value: 'yes', primary: true },
        { label: 'dialog.cancel', value: 'no' },
    ], { name });
    return choice === 'yes';
}

export function aboutDialog(version) {
    openDialog(t('dialog.about.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="about-row">
                <div class="about-mark">${icon('logo', 40)}</div>
                <div class="about-body">
                    <h3>${escapeHtml(t('common.product'))} <span class="muted">v${escapeHtml(version)}</span></h3>
                    <p>${escapeHtml(t('dialog.about.tagline'))}</p>
                    <p class="muted about-line">${escapeHtml(t('dialog.about.stack'))}</p>
                    <p class="muted about-line">${escapeHtml(t('dialog.about.license'))}</p>
                </div>
            </div>`;
        addButton(actions, t('dialog.close'), () => close(null));
    });
}

export function newSceneDialog(onCreate) {
    openDialog(t('dialog.newScene.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label for="d-name">${escapeHtml(t('dialog.newScene.name'))}</label>
                <input type="text" id="d-name" value="${escapeAttr(t('content.newScene'))}" spellcheck="false"></div>`;
        const input = body.querySelector('#d-name');
        input.select();
        const ok = () => { const name = input.value.trim(); if (name) { close(name); onCreate(name); } };
        input.addEventListener('keydown', ev => { if (ev.key === 'Enter') ok(); });
        addButton(actions, t('common.create'), ok, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
    });
}

export function newScriptDialog(project, onCreate) {
    openDialog(t('dialog.newScript.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label for="d-name">${escapeHtml(t('dialog.newScript.name'))}</label>
                <input type="text" id="d-name" placeholder="${escapeAttr(t('dialog.newScript.placeholder'))}" spellcheck="false"></div>
            <div class="form-hint" id="d-hint">${escapeHtml(t('dialog.newScript.hint'))}</div>`;
        const input = body.querySelector('#d-name');
        const hint = body.querySelector('#d-hint');
        const ok = () => {
            const name = input.value.trim();
            // Surface the rejection instead of silently ignoring the keystroke,
            // which is what the previous bare `return` did.
            if (!/^[A-Za-z]\w*$/.test(name)) { hint.textContent = t('dialog.newScript.invalid'); input.focus(); return; }
            close(name);
            onCreate(name);
        };
        input.addEventListener('keydown', ev => { if (ev.key === 'Enter') ok(); });
        addButton(actions, t('common.create'), ok, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
    });
}

export function newProjectDialog() {
    return openDialog(t('dialog.newProject.title'), async (body, close, actions) => {
        const templates = await ipc.listTemplates();
        const byId = Object.fromEntries(templates.map(x => [x.id, x]));
        const order = ['project-3d', 'project-2d'];
        let selected = order.find(id => byId[id]) || templates[0]?.id;
        body.innerHTML = `
            <div class="template-cards" role="radiogroup">${order.filter(id => byId[id]).map(id => `
                <button type="button" class="template-card${id === selected ? ' selected' : ''}" data-id="${id}" role="radio" aria-checked="${id === selected}">
                    <h4>${escapeHtml(t(byId[id].titleKey))}</h4><p>${escapeHtml(t(byId[id].descKey))}</p>
                </button>`).join('')}</div>
            <div class="form-row"><label for="d-name">${escapeHtml(t('dialog.newProject.name'))}</label>
                <input type="text" id="d-name" value="${escapeAttr(t('dialog.newProject.defaultName'))}" spellcheck="false"></div>
            <div class="form-row"><label for="d-dir">${escapeHtml(t('dialog.newProject.location'))}</label>
                <div class="path-row">
                    <input type="text" id="d-dir" spellcheck="false">
                    <button class="btn" id="d-browse">${escapeHtml(t('common.browse'))}</button>
                </div></div>
            <div class="form-hint" id="d-hint"></div>`;

        const nameInput = body.querySelector('#d-name');
        const dirInput = body.querySelector('#d-dir');
        const hint = body.querySelector('#d-hint');
        let parent = '';
        const docs = process.env.HOME || process.env.USERPROFILE || '.';
        parent = path_join(docs, 'CairnProjects');
        try { fs.mkdirSync(parent, { recursive: true }); } catch { /* ignore */ }
        dirInput.value = parent;

        const cards = [...body.querySelectorAll('.template-card')];
        const pick = card => {
            for (const c of cards) {
                const on_ = c === card;
                c.classList.toggle('selected', on_);
                c.setAttribute('aria-checked', on_ ? 'true' : 'false');
            }
            selected = card.dataset.id;
        };
        cards.forEach(card => {
            card.addEventListener('click', () => pick(card));
            card.addEventListener('keydown', ev => {
                if (ev.key === 'ArrowRight' || ev.key === 'ArrowLeft') {
                    ev.preventDefault();
                    const i = cards.indexOf(card);
                    const next = cards[(i + (ev.key === 'ArrowRight' ? 1 : cards.length - 1)) % cards.length];
                    pick(next);
                    next.focus();
                }
            });
        });
        body.querySelector('#d-browse').addEventListener('click', async () => {
            const dir = await ipc.pickDirectory({ title: t('dialog.newProject.location'), defaultPath: parent });
            if (dir) { parent = dir; dirInput.value = dir; }
        });

        const ok = async () => {
            const name = nameInput.value.trim();
            if (!name || !selected || !parent) return;
            const r = await ipc.createProject({ parentDir: parent, name, templateId: selected });
            if (!r.ok) { hint.textContent = r.error; return; }
            close(r.project);
        };
        nameInput.addEventListener('keydown', ev => { if (ev.key === 'Enter') ok(); });
        addButton(actions, t('dialog.newProject.create'), ok, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
        nameInput.select();
    });
}

export function settingsDialog(prefs, onSave) {
    openDialog(t('dialog.settings.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label for="d-lang">${escapeHtml(t('dialog.settings.language'))}</label>
                <select id="d-lang">
                    <option value="en" ${prefs.language === 'en' ? 'selected' : ''}>${escapeHtml(t('common.language.en'))}</option>
                    <option value="zh-CN" ${prefs.language === 'zh-CN' ? 'selected' : ''}>${escapeHtml(t('common.language.zh'))}</option>
                </select></div>
            <div class="form-row"><label for="d-compiler">${escapeHtml(t('dialog.settings.compiler'))}</label>
                <input type="text" id="d-compiler" value="${escapeAttr(prefs.compiler || '')}" placeholder="g++" spellcheck="false"></div>
            <div class="form-hint form-block">${escapeHtml(t('dialog.settings.compilerHint'))}</div>
            <div class="form-row"><label for="d-autosave">${escapeHtml(t('dialog.settings.autosave'))}</label>
                <input type="checkbox" id="d-autosave" ${prefs.autosave ? 'checked' : ''}></div>
            <div class="form-row"><label for="d-stats">${escapeHtml(t('dialog.settings.stats'))}</label>
                <input type="checkbox" id="d-stats" ${prefs.showStats ? 'checked' : ''}></div>`;
        addButton(actions, t('common.ok'), async () => {
            await onSave({
                language: body.querySelector('#d-lang').value,
                compiler: body.querySelector('#d-compiler').value.trim(),
                autosave: body.querySelector('#d-autosave').checked,
                showStats: body.querySelector('#d-stats').checked,
            });
            close(null);
        }, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
    });
}

export function exportDialog(project, onExport) {
    openDialog(t('dialog.export.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label for="d-dir">${escapeHtml(t('dialog.export.folder'))}</label>
                <div class="path-row">
                    <input type="text" id="d-dir" spellcheck="false">
                    <button class="btn" id="d-browse">${escapeHtml(t('common.browse'))}</button>
                </div></div>
            <div class="form-hint form-block">${escapeHtml(t('dialog.export.hint'))}</div>
            <div class="form-hint" id="d-hint"></div>`;
        const dirInput = body.querySelector('#d-dir');
        const hint = body.querySelector('#d-hint');
        dirInput.value = path_join(project.path, 'build', 'player');
        body.querySelector('#d-browse').addEventListener('click', async () => {
            const dir = await ipc.pickDirectory({ title: t('dialog.export.folder'), defaultPath: dirInput.value });
            if (dir) dirInput.value = dir;
        });
        addButton(actions, t('dialog.export.build'), async () => {
            hint.textContent = t('status.compiling');
            const r = await onExport(dirInput.value.trim());
            if (r.ok) close(null);
            else hint.textContent = r.error || t('status.failed');
        }, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
    });
}

function addButton(actions, label, onClick, primary) {
    const btn = document.createElement('button');
    btn.className = 'btn' + (primary ? ' primary' : '');
    btn.textContent = label;
    btn.addEventListener('click', onClick);
    actions.appendChild(btn);
    return btn;
}

// Local path helper (keeps this module free of `path` for dialog use).
function path_join(...parts) { return parts.join('/').replace(/\/+/g, '/'); }

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
function escapeAttr(s) { return escapeHtml(s); }