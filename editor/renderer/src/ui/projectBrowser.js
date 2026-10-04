// Cairn Engine editor - startup project browser.
'use strict';

import fs from 'fs';
import path from 'path';
import { t, onLanguageChange, setLanguage, language as lang } from '../i18n.js';
import { ipc } from '../ipc.js';
import { icon } from '../icons.js';

export class ProjectBrowser {
    constructor(root, { engineVersion, onOpen }) {
        this.root = root;
        this.engineVersion = engineVersion;
        this.onOpen = onOpen;
        this.recent = [];
        this.selected = 0;
        this._offLang = onLanguageChange(() => this.render());
    }

    async render() {
        this.recent = await ipc.recentProjects();
        this.selected = Math.min(this.selected, Math.max(0, this.recent.length - 1));

        const el = document.createElement('div');
        el.id = 'project-browser';
        el.innerHTML = `
            <div class="pb-side">
                <div class="pb-brand">
                    <h1>${icon('logo', 20)} Cairn Engine</h1>
                    <p>v${this.engineVersion} — ${t('dialog.about.tagline')}</p>
                </div>
                <div class="pb-list"></div>
                <div class="pb-actions">
                    <button class="btn primary" data-act="new">${t('menu.file.newProject')}</button>
                    <button class="btn" data-act="open">${t('menu.file.openProject')}</button>
                    <div style="display:flex; gap:8px; align-items:center; margin-top:4px;">
                        <span class="muted" style="font-size:11px;">${t('dialog.settings.language')}</span>
                        <select data-act="lang" style="flex:1; height:24px;">
                            <option value="en" ${lang() === 'en' ? 'selected' : ''}>${t('common.language.en')}</option>
                            <option value="zh-CN" ${lang() === 'zh-CN' ? 'selected' : ''}>${t('common.language.zh')}</option>
                        </select>
                    </div>
                </div>
            </div>
            <div class="pb-main">
                ${icon('logo', 96)}
                <div>${this.recent.length ? t('dialog.confirm') === 'Confirm' ? 'Select a project or create a new one.' : '选择一个项目或新建。' : 'Create your first project to get started.'}</div>
            </div>`;

        const list = el.querySelector('.pb-list');
        if (!this.recent.length) {
            list.innerHTML = `<div class="pb-item muted" style="font-size:11.5px;">${t('content.empty')}</div>`;
        }
        this.recent.forEach((p, i) => {
            const item = document.createElement('div');
            item.className = 'pb-item' + (i === this.selected ? ' selected' : '');
            item.innerHTML = `<div class="name">${escapeHtml(p.name)}</div><div class="path">${escapeHtml(p.path)}</div>`;
            item.addEventListener('click', () => { this.selected = i; this.render(); });
            item.addEventListener('dblclick', () => this.openPath(p.path));
            list.appendChild(item);
        });

        el.querySelector('[data-act=new]').addEventListener('click', async () => {
            const { newProjectDialog } = await import('./dialogs.js');
            const created = await newProjectDialog();
            if (created) this.openPath(created.path);
        });
        el.querySelector('[data-act=open]').addEventListener('click', () => this.browse());
        el.querySelector('[data-act=lang]').addEventListener('change', async ev => {
            setLanguage(ev.target.value);
            await import('../prefs.js').then(m => m.setLanguagePref(ev.target.value));
        });

        this.root.innerHTML = '';
        this.root.appendChild(el);
        this.el = el;
    }

    async browse() {
        const dir = await ipc.pickDirectory({ title: t('menu.file.openProject') });
        if (dir) this.openPath(dir);
    }

    async openPath(projectDir) {
        const r = await ipc.readProject(projectDir);
        if (!r.ok) {
            await ipc.messageBox({ type: 'error', message: r.error, title: 'Cairn Engine' });
            return;
        }
        this._offLang();
        this.onOpen(r.project);
    }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
