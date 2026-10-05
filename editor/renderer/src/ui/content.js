// Cairn Engine editor - content browser.
// File explorer over <project>/Assets with thumbnails, open actions and
// create/import/rename/delete operations.
//
// Like the hierarchy, all row events are delegated from two containers that
// are created once. Re-binding a contextmenu handler on every render used to
// stack one extra listener per refresh on the same surviving element.
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
const CODE_EXT = new Set(['.cpp', '.h', '.hpp', '.cc', '.cxx', '.inl']);
const TILE_ICON = 20;   // drawn from the 14/16/20 icon grid

export class ContentBrowser {
    constructor(pageEl, openFile) {
        this.pageEl = pageEl;
        this.openFile = openFile;
        this.rel = '';
        this.tiles = [];
        pageEl.innerHTML = `
            <div class="content-wrap">
                <div class="content-side" id="cb-folders" role="tree"></div>
                <div class="content-main">
                    <div class="content-crumbs" id="cb-crumbs"></div>
                    <div class="content-grid" id="cb-grid"></div>
                </div>
            </div>`;
        this.foldersEl = pageEl.querySelector('#cb-folders');
        this.crumbsEl = pageEl.querySelector('#cb-crumbs');
        this.gridEl = pageEl.querySelector('#cb-grid');

        this.bind();

        on('project-opened', () => { this.rel = ''; this.render(); });
        this.render();
    }

    refresh() { this.render(); }

    assetsRoot() { return path.join(editor.project.path, 'Assets'); }
    currentDir() { return path.join(this.assetsRoot(), this.rel); }

    // ---- events (bound once) --------------------------------------------------
    bind() {
        this.foldersEl.addEventListener('click', ev => {
            const row = ev.target.closest('.folder-row');
            if (!row) return;
            this.rel = row.dataset.rel;
            this.render();
        });
        this.foldersEl.addEventListener('keydown', ev => {
            const row = ev.target.closest('.folder-row');
            if (!row) return;
            if (ev.key === 'Enter' || ev.key === ' ') {
                ev.preventDefault();
                this.rel = row.dataset.rel;
                this.render();
            } else if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
                ev.preventDefault();
                const rows = [...this.foldersEl.querySelectorAll('.folder-row')];
                const i = rows.indexOf(row);
                const next = rows[Math.max(0, Math.min(rows.length - 1, i + (ev.key === 'ArrowDown' ? 1 : -1)))];
                next && next.focus();
            }
        });
        this.foldersEl.addEventListener('contextmenu', ev => {
            if (ev.target.closest('.folder-row')) return;
            ev.preventDefault();
            this.folderMenu(ev);
        });

        // Tile grid: arrows move, Enter opens, F2 renames - parity with the
        // dblclick / right-click pair.
        this.gridEl.addEventListener('click', ev => {
            const tile = ev.target.closest('.asset-tile');
            if (tile) this.selectTile(tile);
        });
        this.gridEl.addEventListener('dblclick', ev => {
            const tile = ev.target.closest('.asset-tile');
            if (tile && tile._open) tile._open();
        });
        this.gridEl.addEventListener('keydown', ev => this.onTileKey(ev));
        this.gridEl.addEventListener('contextmenu', ev => {
            const tile = ev.target.closest('.asset-tile');
            if (tile) {
                ev.preventDefault();
                ev.stopPropagation();
                this.selectTile(tile);
                this.assetMenu(ev, tile.dataset.name, tile.dataset.dir === '1');
            } else {
                ev.preventDefault();
                this.folderMenu(ev);
            }
        });
        this.crumbsEl.addEventListener('click', ev => {
            const c = ev.target.closest('.crumb');
            if (!c) return;
            this.rel = c.dataset.rel;
            this.render();
        });
    }

    selectTile(tile) {
        for (const t2 of this.tiles) t2.classList.toggle('selected', t2 === tile);
    }

    onTileKey(ev) {
        const tile = ev.target.closest('.asset-tile');
        if (!tile) return;
        const cols = Math.max(1, Math.floor(this.gridEl.clientWidth / 96));
        const i = this.tiles.indexOf(tile);
        let next = -1;
        if (ev.key === 'ArrowRight') next = Math.min(this.tiles.length - 1, i + 1);
        else if (ev.key === 'ArrowLeft') next = Math.max(0, i - 1);
        else if (ev.key === 'ArrowDown') next = Math.min(this.tiles.length - 1, i + cols);
        else if (ev.key === 'ArrowUp') next = Math.max(0, i - cols);
        else if (ev.key === 'Home') next = 0;
        else if (ev.key === 'End') next = this.tiles.length - 1;
        else if (ev.key === 'Enter') { ev.preventDefault(); tile._open && tile._open(); return; }
        else if (ev.key === 'F2') { ev.preventDefault(); this.startRename(tile); return; }
        else return;
        ev.preventDefault();
        if (next >= 0) { this.selectTile(this.tiles[next]); this.tiles[next].focus(); }
    }

    // ---- render ---------------------------------------------------------------
    render() {
        this.renderFolders();
        this.renderCrumbs();
        this.renderGrid();
    }

    renderFolders() {
        const host = this.foldersEl;
        const frag = document.createDocumentFragment();
        frag.appendChild(this.folderRow(t('content.assets'), '', 0));
        const walk = (dir, rel, depth) => {
            let entries;
            try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
            for (const e of entries.filter(x => x.isDirectory() && x.name !== 'Library')
                .sort((a, b) => a.name.localeCompare(b.name))) {
                const childRel = rel ? rel + '/' + e.name : e.name;
                frag.appendChild(this.folderRow(e.name, childRel, depth));
                walk(path.join(dir, e.name), childRel, depth + 1);
            }
        };
        walk(this.assetsRoot(), '', 1);
        host.innerHTML = '';
        host.appendChild(frag);
    }

    folderRow(label, rel, depth) {
        const row = document.createElement('div');
        row.className = 'folder-row' + (rel === this.rel ? ' selected' : '');
        row.dataset.rel = rel;
        row.dataset.depth = String(depth);
        row.tabIndex = -1;
        row.setAttribute('role', 'treeitem');
        row.innerHTML = `<span class="folder-icon">${icon('folder')}</span>` +
            `<span class="name" title="${escapeAttr(label)}">${escapeHtml(label)}</span>`;
        return row;
    }

    renderCrumbs() {
        const host = this.crumbsEl;
        const parts = this.rel ? this.rel.split('/') : [];
        const frag = document.createDocumentFragment();
        frag.appendChild(this.crumb(t('content.assets'), '', true));
        let acc = '';
        for (const p of parts) {
            acc = acc ? acc + '/' + p : p;
            const sep = document.createElement('span');
            sep.className = 'sep';
            sep.textContent = '›';
            frag.appendChild(sep);
            frag.appendChild(this.crumb(p, acc, false));
        }
        host.innerHTML = '';
        host.appendChild(frag);
    }

    crumb(label, rel, isRoot) {
        const c = document.createElement('span');
        c.className = 'crumb' + (isRoot && !this.rel ? ' current' : '');
        c.dataset.rel = rel;
        c.textContent = label;
        c.title = label;
        return c;
    }

    renderGrid() {
        const grid = this.gridEl;
        let entries;
        try { entries = fs.readdirSync(this.currentDir(), { withFileTypes: true }); }
        catch { grid.innerHTML = ''; this.tiles = []; return; }
        const dirs = entries.filter(e => e.isDirectory() && e.name !== 'Library')
            .sort((a, b) => a.name.localeCompare(b.name));
        const files = entries.filter(e => e.isFile())
            .sort((a, b) => a.name.localeCompare(b.name));

        if (!dirs.length && !files.length) {
            grid.innerHTML = `<div class="empty-line">${escapeHtml(t('content.empty'))}</div>`;
            this.tiles = [];
            return;
        }

        const frag = document.createDocumentFragment();
        const tiles = [];
        for (const d of dirs) {
            tiles.push(this.tile({
                name: d.name,
                isDir: true,
                thumb: icon('folder', TILE_ICON),
                open: () => { this.rel = this.rel ? this.rel + '/' + d.name : d.name; this.render(); },
            }));
        }
        for (const f of files) tiles.push(this.assetTile(f.name));
        for (const tile of tiles) frag.appendChild(tile);
        grid.innerHTML = '';
        grid.appendChild(frag);
        this.tiles = tiles;
    }

    tile({ name, isDir, thumb, imgSrc, open }) {
        const el = document.createElement('div');
        el.className = 'asset-tile';
        el.dataset.name = name;
        el.dataset.dir = isDir ? '1' : '0';
        el.tabIndex = -1;
        el._open = open;
        const img = imgSrc
            ? `<img draggable="false" alt="" src="${escapeAttr(imgSrc)}">`
            : thumb;
        el.innerHTML = `<div class="thumb">${img}</div>` +
            `<div class="name" title="${escapeAttr(name)}">${escapeHtml(name)}</div>`;
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
            return this.tile({ name, thumb: icon('script', TILE_ICON), open: () => this.openFile(full) });
        }
        if (ext === '.cscene') {
            return this.tile({ name, thumb: icon('scene', TILE_ICON), open: () => emit('open-scene', full) });
        }
        const ic = ext === '.obj' ? 'mesh' : ext === '.wav' ? 'physics' : 'image';
        return this.tile({ name, thumb: icon(ic, TILE_ICON), open: () => {} });
    }

    // Inline rename on the tile, matching the hierarchy. The old flow went
    // through window.prompt, which is a blocking native dialog with an
    // unlocalised title and no way to cancel an in-progress edit with Escape
    // without destroying it.
    startRename(tile) {
        const name = tile.dataset.name;
        const isDir = tile.dataset.dir === '1';
        const full = path.join(this.currentDir(), name);
        const label = tile.querySelector('.name');
        if (!label) return;
        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'tile-rename';
        input.value = name;
        input.spellcheck = false;
        label.replaceWith(input);
        input.focus();
        const dot = name.lastIndexOf('.');
        input.setSelectionRange(0, isDir || dot <= 0 ? name.length : dot);
        let done = false;
        const finish = commit => {
            if (done) return;
            done = true;
            const next = input.value.trim();
            if (commit && next && next !== name) this.applyRename(full, next);
            else this.renderGrid();
        };
        input.addEventListener('keydown', ev => {
            if (ev.key === 'Enter') { ev.preventDefault(); finish(true); }
            else if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
        });
        input.addEventListener('blur', () => finish(true));
    }

    applyRename(full, next) {
        const ext = path.extname(full);
        // Renaming to a bare name should not silently drop the extension the
        // editor will use to pick a tile icon.
        const dest = path.join(path.dirname(full), path.extname(next) ? next : next + ext);
        if (dest === full) { this.renderGrid(); return; }
        if (fs.existsSync(dest)) { this.renderGrid(); return; }
        try {
            fs.renameSync(full, dest);
        } catch {
            // In use, or permission denied: leave the row as it was rather
            // than showing a name that does not exist on disk.
        }
        this.render();
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
        const ext = path.extname(name).toLowerCase();
        const items = [];
        if (!isDir && CODE_EXT.has(ext)) items.push({ label: 'ctx.open', run: () => this.openFile(full) });
        if (!isDir && ext === '.cscene') items.push({ label: 'ctx.open', run: () => emit('open-scene', full) });
        items.push(
            { label: 'ctx.rename', run: () => {
                const tile = this.tiles.find(x => x.dataset.name === name);
                if (tile) this.startRename(tile);
            } },
            { label: 'ctx.delete', run: async () => {
                if (await deleteConfirm('asset', name)) {
                    fs.rmSync(full, { recursive: isDir, force: true });
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
        const name = uniqueName(this.currentDir(), t('content.newFolder'), '');
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
        const name = uniqueName(this.currentDir(), t('content.newScene'), '.cscene');
        const file = path.join(this.currentDir(), name + '.cscene');
        fs.writeFileSync(file, JSON.stringify({ format: 'cairn-scene', version: 1, name, entities: [] }, null, 2));
        emit('open-scene', file);
        this.render();
    }

    async importTexture() {
        const src = await ipc.pickOpenFile({
            title: t('ctx.importTexture'),
            filters: [{ name: t('content.imageFilter'), extensions: ['png', 'jpg', 'jpeg', 'bmp'] }],
        });
        if (!src) return;
        const dest = path.join(this.currentDir(), path.basename(src));
        fs.copyFileSync(src, dest);
        this.render();
    }

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
function escapeAttr(s) { return escapeHtml(s); }