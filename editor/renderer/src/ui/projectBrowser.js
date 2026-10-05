// Cairn Engine editor - startup project browser.
//
// A startup screen, not a landing page. One rail, one list, one quiet line of
// guidance in the main area. No centred hero, no oversized mark, no marketing
// copy, and no "Welcome to X".
'use strict';

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
        this.el = null;
        this.bind();
    }

    // One set of delegated handlers, bound before the first render, so a
    // language switch does not re-register anything.
    bind() {
        this.root.addEventListener('click', async ev => {
            const act = ev.target.closest('[data-act]');
            if (act) {
                ev.stopPropagation();
                if (act.dataset.act === 'new') {
                    const { newProjectDialog } = await import('./dialogs.js');
                    const created = await newProjectDialog();
                    if (created) this.openPath(created.path);
                } else if (act.dataset.act === 'open') {
                    this.browse();
                }
                return;
            }
            const item = ev.target.closest('.pb-item');
            if (!item) return;
            this.selected = Number(item.dataset.i);
            this.paintSelection();
        });
        this.root.addEventListener('dblclick', ev => {
            const item = ev.target.closest('.pb-item');
            if (item) this.openPath(this.recent[Number(item.dataset.i)].path);
        });
        this.root.addEventListener('keydown', ev => {
            const item = ev.target.closest('.pb-item');
            if (item && ev.key === 'Enter') {
                ev.preventDefault();
                this.openPath(this.recent[Number(item.dataset.i)].path);
                return;
            }
            if (ev.key !== 'ArrowDown' && ev.key !== 'ArrowUp') return;
            ev.preventDefault();
            const items = [...this.root.querySelectorAll('.pb-item')];
            if (!items.length) return;
            const i = items.indexOf(document.activeElement);
            const next = Math.max(0, Math.min(items.length - 1,
                (i < 0 ? -1 : i) + (ev.key === 'ArrowDown' ? 1 : -1)));
            this.selected = next;
            this.paintSelection();
            items[next].focus({ preventScroll: false });
        });
        this.root.addEventListener('change', ev => {
            if (ev.target.dataset.act === 'lang') {
                setLanguage(ev.target.value);
                import('../prefs.js').then(m => m.setLanguagePref(ev.target.value));
            }
        });
    }

    paintSelection() {
        if (!this.el) return;
        for (const node of this.el.querySelectorAll('.pb-item')) {
            const on_ = Number(node.dataset.i) === this.selected;
            node.classList.toggle('selected', on_);
            node.setAttribute('aria-selected', on_ ? 'true' : 'false');
        }
    }

    async render() {
        this.recent = await ipc.recentProjects();
        this.selected = Math.min(this.selected, Math.max(0, this.recent.length - 1));

        const el = document.createElement('div');
        el.id = 'project-browser';
        el.innerHTML = `
            <div class="pb-side">
                <div class="pb-brand">
                    <h1>${icon('logo', 20)}<span>${escapeHtml(t('dialog.about.title'))}</span></h1>
                    <p>v${escapeHtml(this.engineVersion)}</p>
                </div>
                <div class="pb-list" role="listbox"></div>
                <div class="pb-actions">
                    <button class="btn primary" data-act="new">${escapeHtml(t('menu.file.newProject'))}</button>
                    <button class="btn" data-act="open">${escapeHtml(t('menu.file.openProject'))}</button>
                </div>
                <div class="pb-foot">
                    <label for="pb-lang">${escapeHtml(t('dialog.settings.language'))}</label>
                    <select id="pb-lang" data-act="lang">
                        <option value="en" ${lang() === 'en' ? 'selected' : ''}>${escapeHtml(t('common.language.en'))}</option>
                        <option value="zh-CN" ${lang() === 'zh-CN' ? 'selected' : ''}>${escapeHtml(t('common.language.zh'))}</option>
                    </select>
                </div>
            </div>
            <div class="pb-main">
                <div>${escapeHtml(this.recent.length ? t('projectBrowser.select') : t('projectBrowser.first'))}</div>
                <div class="hint">${escapeHtml(t('projectBrowser.hint'))}</div>
            </div>`;

        const list = el.querySelector('.pb-list');
        if (!this.recent.length) {
            list.innerHTML = `<div class="empty-line">${escapeHtml(t('projectBrowser.none'))}</div>`;
        } else {
            const frag = document.createDocumentFragment();
            this.recent.forEach((p, i) => {
                const item = document.createElement('div');
                item.className = 'pb-item' + (i === this.selected ? ' selected' : '');
                item.dataset.i = String(i);
                item.tabIndex = 0;
                item.setAttribute('role', 'option');
                item.setAttribute('aria-selected', i === this.selected ? 'true' : 'false');
                item.title = p.path;
                item.innerHTML =
                    `<div class="name">${escapeHtml(p.name)}</div><div class="path">${escapeHtml(p.path)}</div>`;
                frag.appendChild(item);
            });
            list.appendChild(frag);
        }

        // Swap contents instead of replacing the node, so focus survives a
        // language change.
        if (this.el) { this.el.replaceWith(el); } else { this.root.innerHTML = ''; this.root.appendChild(el); }
        this.el = el;
    }

    async browse() {
        const dir = await ipc.pickDirectory({ title: t('menu.file.openProject') });
        if (dir) this.openPath(dir);
    }

    async openPath(projectDir) {
        const r = await ipc.readProject(projectDir);
        if (!r.ok) {
            await ipc.messageBox({ type: 'error', message: r.error, title: t('dialog.about.title') });
            return;
        }
        this._offLang();
        this.onOpen(r.project);
    }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}