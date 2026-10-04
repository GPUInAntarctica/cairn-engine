// Cairn Engine editor - content browser.
// File explorer over <project>/Assets with thumbnails, open actions and
// create/import/rename/delete operations.
'use strict';

import fs from 'fs';
import path from 'path';

import { editor } from '../state.js';
import { on, emit } from '../events.js';
import { t } from '../i18n.js';
import { icon } from '../icons.js';
import { ipc } from '../ipc.js';
import { showContextMenu } from './contextmenu.js';
import { deleteConfirm, newScriptDialog } from './dialogs.js';

const IMAGE_EXT = new Set(['.png', '.jpg', '.jpeg', '.bmp']);
const SCRIPT_EXT = new Set(['.cpp', '.h', '.hpp']);
const CODE_EXT = new Set(['.cpp', '.h', '.hpp', '.cc', '.cxx', '.inl']);

export class ContentBrowser {
    constructor(pageEl, openFile) {
        this.pageEl = pageEl;
        this.openFile = openFile;      // (absPath) => opens code/image tab
        this.rel = '';                 // folder relative to Assets
        pageEl.innerHTML = `
            <div class="content-wrap">
                <div class="content-side" id="cb-folders"></div>
                <div class="content-main">
                    <div class="content-crumbs" id="cb-crumbs"></div>
                    <div class="content-grid" id="cb-grid"></div>
                </div>
            </div>`;
        on('project-opened', () => { this.rel = ''; this.render(); });
        this.render();
    }

    refresh() { this.render(); }

    assetsRoot() { return path.join(editor.project.path, 'Assets'); }
    currentDir() { return path.join(this.assetsRoot(), this.rel); }

    render() {
        this.renderFolders();
        this.renderCrumbs();
        this.renderGrid();
    }

    renderFolders() {
        const host = this.pageEl.querySelector('#cb-folders');
        host.innerHTML = '';
        const rootRow = folderRow(icon('folder'), t('content.assets'), '');
        host.appendChild(rootRow);
        const walk = (dir, rel, depth) => {
            let entries;
            try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
            for (const e of entries.filter(x => x.isDirectory()).sort((a, b) => a.name.localeCompare(b.name))) {
                if (e.name === 'Library') continue;
                const childRel = rel ? rel + '/' + e.name : e.name;
                const row = folderRow(icon('folder'), e.name, childRel);
                row.style.paddingLeft = (10 + depth * 14) + 'px';
                host.appendChild(row);
                walk(path.join(dir, e.name), childRel, depth + 1);
            }
        };
        walk(this.assetsRoot(), '', 1);
        host.addEventListener('contextmenu', ev => {
            if (ev.target.closest('.folder-row')) return;
            ev.preventDefault();
            this.folderMenu(ev);
        });
    }

    renderCrumbs() {
        const host = this.pageEl.querySelector('#cb-crumbs');
        host.innerHTML = '';
        const parts = this.rel ? this.rel.split('/') : [];
        const mk = (label, rel) => {
            const c = document.createElement('span');
            c.className = 'crumb';
            c.textContent = label;
            c.addEventListener('click', () => { this.rel = rel; this.render(); });
            host.appendChild(c);
        };
        mk(t('content.assets'), '');
        let acc = '';
        for (const p of parts) {
            host.appendChild(Object.assign(document.createElement('span'), { textContent: '›' }));
            acc = acc ? acc + '/' + p : p;
            mk(p, acc);
        }
    }

    renderGrid() {
        const grid = this.pageEl.querySelector('#cb-grid');
        grid.innerHTML = '';
        let entries;
        try { entries = fs.readdirSync(this.currentDir(), { withFileTypes: true }); }
        catch { return; }
        const dirs = entries.filter(e => e.isDirectory() && e.name !== 'Library').sort((a, b) => a.name.localeCompare(b.name));
        const files = entries.filter(e => e.isFile()).sort((a, b) => a.name.localeCompare(b.name));

        for (const d of dirs) {
            grid.appendChild(this.tile({
                name: d.name,
                isDir: true,
                thumb: icon('folder', 26),
                open: () => { this.rel = this.rel ? this.rel + '/' + d.name : d.name; this.render(); },
            }));
        }
        for (const f of files) {
            grid.appendChild(this.assetTile(f.name));
        }
        if (!dirs.length && !files.length) {
            grid.innerHTML = `<div class="muted" style="grid-column:1/-1; padding:8px;">${t('content.empty')}</div>`;
        }

        grid.addEventListener('contextmenu', ev => {
            if (ev.target.closest('.asset-tile')) return;
            ev.preventDefault();
            this.folderMenu(ev);
        }, { once: false });
    }

    tile({ name, isDir, thumb, imgSrc, open }) {
        const el = document.createElement('div');
        el.className = 'asset-tile';
        el.innerHTML = `<div class="thumb">${imgSrc ? `<img draggable="false" src="${imgSrc}">` : thumb}</div><div class="name">${escapeHtml(name)}</div>`;
        el.addEventListener('dblclick', () => open && open());
        el.addEventListener('click', ev => {
            document.querySelectorAll('.asset-tile.selected').forEach(x => x.classList.remove('selected'));
            el.classList.add('selected');
        });
        el.addEventListener('contextmenu', ev => {
            ev.preventDefault();
            ev.stopPropagation();
            document.querySelectorAll('.asset-tile.selected').forEach(x => x.classList.remove('selected'));
            el.classList.add('selected');
            this.assetMenu(ev, name, isDir);
        });
        return el;
    }

    assetTile(name) {
        const ext = path.extname(name).toLowerCase();
        const full = path.join(this.currentDir(), name);
        if (IMAGE_EXT.has(ext)) {
            return this.tile({
                name,
                thumb: '',
                imgSrc: 'file://' + full.replace(/\\/g, '/').replace(/#/g, '%23').replace(/\?/g, '%3F'),
                open: () => this.openFile(full),
            });
        }
        if (CODE_EXT.has(ext)) {
            return this.tile({ name, thumb: icon('script', 26), open: () => this.openFile(full) });
        }
        if (ext === '.cscene') {
            return this.tile({ name, thumb: icon('scene', 26), open: () => emit('open-scene', full) });
        }
        const ic = ext === '.obj' ? 'mesh' : ext === '.wav' ? 'physics' : 'image';
        return this.tile({ name, thumb: icon(ic, 26), open: () => {} });
    }

    // ---- menus -------------------------------------------------------------
    folderMenu(ev) {
        showContextMenu(ev.clientX, ev.clientY, [
            { label: 'ctx.newFolder', run: () => this.newFolder() },
            { label: 'ctx.newScript', run: () => newScriptDialog(editor.project, name => this.newScript(name)) },
            { label: 'ctx.newScene', run: () => this.newScene() },
            '-',
            { label: 'ctx.importTexture', run: () => this.importTexture() },
            { label: 'ctx.showInFolder', run: () => ipc.showItemInFolder(this.currentDir()) },
        ]);
    }

    assetMenu(ev, name, isDir) {
        const full = path.join(this.currentDir(), name);
        const items = [];
        if (!isDir && CODE_EXT.has(path.extname(name).toLowerCase())) {
            items.push({ label: 'ctx.open', run: () => this.openFile(full) });
        }
        if (!isDir && path.extname(name).toLowerCase() === '.cscene') {
            items.push({ label: 'ctx.open', run: () => emit('open-scene', full) });
        }
        items.push(
            { label: 'ctx.rename', run: () => this.rename(full, name, isDir) },
            { label: 'ctx.delete', run: async () => {
                if (await deleteConfirm('asset', name)) {
                    if (isDir) fs.rmSync(full, { recursive: true, force: true });
                    else fs.rmSync(full, { force: true });
                    this.render();
                }
            } },
            '-',
            { label: 'ctx.showInFolder', run: () => ipc.showItemInFolder(full) },
        );
        showContextMenu(ev.clientX, ev.clientY, items);
    }

    // ---- operations ----------------------------------------------------------
    newFolder() {
        const name = uniqueName(this.currentDir(), 'New Folder', '');
        fs.mkdirSync(path.join(this.currentDir(), name));
        this.render();
    }

    newScript(className) {
        const file = path.join(this.currentDir(), className + '.cpp');
        fs.writeFileSync(file, scriptTemplate(className));
        emit('open-code', file);
        this.render();
    }

    newScene() {
        const name = uniqueName(this.currentDir(), 'Scene', '.cscene');
        const file = path.join(this.currentDir(), name + '.cscene');
        fs.writeFileSync(file, JSON.stringify({ format: 'cairn-scene', version: 1, name, entities: [] }, null, 2));
        emit('open-scene', file);
        this.render();
    }

    async importTexture() {
        const src = await ipc.pickOpenFile({
            title: t('ctx.importTexture'),
            filters: [{ name: 'Images', extensions: ['png', 'jpg', 'jpeg', 'bmp'] }],
        });
        if (!src) return;
        const dest = path.join(this.currentDir(), path.basename(src));
        fs.copyFileSync(src, dest);
        this.render();
    }

    rename(full, name, isDir) {
        const input = window.prompt('Rename', name);
        if (!input || input === name) return;
        const dest = path.join(path.dirname(full), input);
        try { fs.renameSync(full, dest); } catch (err) { /* in use */ }
        this.render();
    }
}

function folderRow(ic, label, rel) {
    const row = document.createElement('div');
    row.className = 'folder-row';
    row.innerHTML = `${ic}<span>${escapeHtml(label)}</span>`;
    row.addEventListener('click', () => {
        // handled by closure below through dataset
        row.dispatchEvent(new CustomEvent('navigate', { bubbles: true, detail: rel }));
    });
    row.addEventListener('navigate', ev => {});
    row.addEventListener('click', () => {}, { once: false });
    row.dataset.rel = rel;
    row.addEventListener('click', () => {
        const host = row.closest('.content-side');
        const browser = host && host.parentElement && host.parentElement['__browser'];
        if (browser) { browser.rel = rel; browser.render(); }
    });
    return row;
}

function uniqueName(dir, base, ext) {
    let name = base + ext;
    let i = 1;
    while (fs.existsSync(path.join(dir, name))) name = `${base} ${++i}${ext}`;
    return name;
}

export function scriptTemplate(className) {
    return `// ${className} - Cairn Engine gameplay script.
// Lifecycle: OnStart (once) -> FixedUpdate (physics rate) / OnUpdate (per frame)
// -> OnDestroy. Inspector fields are declared in DescribeProps and read via
// Prop*() helpers. Register the class once at the bottom.
#include <cairn/cairn.h>
using namespace Cairn;

class ${className} : public Component {
public:
    void OnStart() override {
        Debug::Log("${className}::OnStart");
    }

    void OnUpdate(float deltaTime) override {
        // Example: rotate around Y at 90 degrees per second.
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += 90.0f * deltaTime;
        transform().SetEulerDegrees(euler);
    }

    void FixedUpdate(float fixedDeltaTime) override {}

    void OnTriggerEnter(Component* other) override {}
    void OnTriggerExit(Component* other) override {}

    void OnDestroy() override {}

    // Inspector-editable fields.
    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 5.0f);
    }
};

CAIRN_SCRIPT(${className})
`;
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
