// Cairn Engine editor - modal dialogs.
'use strict';

import fs from 'fs';
import path from 'path';

import { t } from '../i18n.js';
import { ipc } from '../ipc.js';
import { icon } from '../icons.js';

export function openDialog(title, buildBody) {
    const backdrop = document.createElement('div');
    backdrop.className = 'dialog-backdrop';
    const dlg = document.createElement('div');
    dlg.className = 'dialog';
    dlg.innerHTML = `
        <div class="dialog-title"><span>${title}</span><span class="close">${icon('close')}</span></div>
        <div class="dialog-body"></div>
        <div class="dialog-actions"></div>`;
    backdrop.appendChild(dlg);
    document.body.appendChild(backdrop);

    const close = value => { backdrop.remove(); if (resolver) resolver(value); };
    let resolver = null;
    const promise = new Promise(resolve => { resolver = resolve; });

    dlg.querySelector('.close').addEventListener('click', () => close(null));
    backdrop.addEventListener('mousedown', ev => { if (ev.target === backdrop) close(null); });

    const actions = dlg.querySelector('.dialog-actions');
    const body = dlg.querySelector('.dialog-body');
    buildBody(body, close, actions);
    return promise;
}

export function confirmDialog(messageKey, buttons, vars) {
    return openDialog(t('dialog.confirm'), (body, close, actions) => {
        body.innerHTML = `<div style="max-width:420px; line-height:1.6;">${t(messageKey, vars)}</div>`;
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
            <div style="display:flex; gap:18px; align-items:flex-start;">
                <div style="color:var(--accent); padding-top:2px;">${icon('logo', 44)}</div>
                <div style="max-width:420px; line-height:1.7;">
                    <h3 style="font-size:15px; margin-bottom:4px;">Cairn Engine <span class="muted" style="font-weight:400;">v${version}</span></h3>
                    <p>${t('dialog.about.tagline')}</p>
                    <p class="muted" style="margin-top:8px;">C++ · Electron · Software renderer</p>
                    <p class="muted" style="margin-top:8px;">${t('dialog.about.license')}</p>
                </div>
            </div>`;
        const btn = document.createElement('button');
        btn.className = 'btn';
        btn.textContent = t('dialog.close');
        btn.addEventListener('click', () => close(null));
        actions.appendChild(btn);
    });
}

export function newSceneDialog(onCreate) {
    openDialog(t('dialog.newScene.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label>${t('dialog.newScene.name')}</label>
                <input type="text" id="d-name" value="Scene" spellcheck="false"></div>`;
        const input = body.querySelector('#d-name');
        setTimeout(() => { input.focus(); input.select(); }, 30);
        const ok = () => { const name = input.value.trim(); if (name) { close(name); onCreate(name); } };
        input.addEventListener('keydown', ev => { if (ev.key === 'Enter') ok(); });
        addButton(actions, t('common.create'), ok, true);
        addButton(actions, t('dialog.cancel'), () => close(null));
    });
}

export function newScriptDialog(project, onCreate) {
    openDialog(t('dialog.newScript.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label>${t('dialog.newScript.name')}</label>
                <input type="text" id="d-name" placeholder="PlayerController" spellcheck="false"></div>
            <div class="form-hint">${t('dialog.newScript.hint')}</div>`;
        const input = body.querySelector('#d-name');
        setTimeout(() => input.focus(), 30);
        const ok = () => {
            const name = input.value.trim();
            if (!name || !/^[A-Z]\w*$/.test(name)) return;
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
            <div class="template-cards">${order.filter(id => byId[id]).map(id => `
                <div class="template-card ${id === selected ? 'selected' : ''}" data-id="${id}">
                    <h4>${t(byId[id].titleKey)}</h4><p>${t(byId[id].descKey)}</p>
                </div>`).join('')}</div>
            <div class="form-row"><label>${t('dialog.newProject.name')}</label>
                <input type="text" id="d-name" value="MyGame" spellcheck="false"></div>
            <div class="form-row"><label>${t('dialog.newProject.location')}</label>
                <div style="display:flex; gap:6px;">
                    <input type="text" id="d-dir" style="flex:1;" spellcheck="false">
                    <button class="btn" id="d-browse">${t('common.browse')}</button>
                </div></div>
            <div class="form-hint" id="d-hint"></div>`;

        const nameInput = body.querySelector('#d-name');
        const dirInput = body.querySelector('#d-dir');
        const hint = body.querySelector('#d-hint');
        let parent = '';
        const docs = process.env.HOME || process.env.USERPROFILE || '.';
        parent = path_join(docs, 'CairnProjects');
        try { fs_mkdir(parent); } catch { /* ignore */ }
        dirInput.value = parent;

        body.querySelectorAll('.template-card').forEach(card => {
            card.addEventListener('click', () => {
                body.querySelectorAll('.template-card').forEach(c => c.classList.remove('selected'));
                card.classList.add('selected');
                selected = card.dataset.id;
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
        setTimeout(() => nameInput.focus(), 30);
    });
}

export function settingsDialog(prefs, onSave) {
    openDialog(t('dialog.settings.title'), (body, close, actions) => {
        body.innerHTML = `
            <div class="form-row"><label>${t('dialog.settings.language')}</label>
                <select id="d-lang">
                    <option value="en" ${prefs.language === 'en' ? 'selected' : ''}>${t('common.language.en')}</option>
                    <option value="zh-CN" ${prefs.language === 'zh-CN' ? 'selected' : ''}>${t('common.language.zh')}</option>
                </select></div>
            <div class="form-row"><label>${t('dialog.settings.compiler')}</label>
                <input type="text" id="d-compiler" value="${prefs.compiler || ''}" placeholder="g++" spellcheck="false"></div>
            <div class="form-hint" style="margin-bottom:10px;">${t('dialog.settings.compilerHint')}</div>
            <div class="form-row"><label>${t('dialog.settings.autosave')}</label>
                <input type="checkbox" id="d-autosave" ${prefs.autosave ? 'checked' : ''}></div>
            <div class="form-row"><label>${t('dialog.settings.stats')}</label>
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
            <div class="form-row"><label>${t('dialog.export.folder')}</label>
                <div style="display:flex; gap:6px;">
                    <input type="text" id="d-dir" style="flex:1;" spellcheck="false">
                    <button class="btn" id="d-browse">${t('common.browse')}</button>
                </div></div>
            <div class="form-hint" style="margin-bottom:10px;">${t('dialog.export.hint')}</div>
            <div class="form-hint" id="d-hint"></div>`;
        const dirInput = body.querySelector('#d-dir');
        const hint = body.querySelector('#d-hint');
        dirInput.value = path_join(project.path, 'build', 'player');
        body.querySelector('#d-browse').addEventListener('click', async () => {
            const dir = await ipc.pickDirectory({ defaultPath: dirInput.value });
            if (dir) dirInput.value = dir;
        });
        addButton(actions, t('dialog.export.build'), async () => {
            hint.textContent = t('status.compiling');
            const r = await onExport(dirInput.value.trim());
            if (r.ok) close(null);
            else hint.textContent = r.error || 'failed';
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
}

// Local path helpers (keep this module import-free for reuse in dialogs).
function path_join(...parts) { return parts.join('/').replace(/\/+/g, '/'); }
function fs_mkdir(p) { fs.mkdirSync(p, { recursive: true }); }
