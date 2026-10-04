// Cairn Engine editor - main window composition and frame loop.
'use strict';

import path from 'path';
import fs from 'fs';

import { t, setLanguage, language, onLanguageChange } from '../i18n.js';
import { ipc } from '../ipc.js';
import { icon } from '../icons.js';
import {
    editor, engineInstance, openProject, saveScene, openScene, newScene,
    undo, redo, selectEntity, entityList, duplicateEntity, destroyEntity,
    markDirty, setScriptClasses, scriptClassMeta, sceneName, loadAddon,
} from '../state.js';
import { on, emit } from '../events.js';
import { MenuBar } from './menubar.js';
import { Toolbar } from './toolbar.js';
import { Viewport } from './viewport.js';
import { Hierarchy } from './hierarchy.js';
import { Inspector } from './inspector.js';
import { ConsolePanel } from './console.js';
import { ContentBrowser, scriptTemplate } from './content.js';
import { TabHost } from './tabs.js';
import { StatusBar } from './statusbar.js';
import {
    aboutDialog, settingsDialog, newSceneDialog, newScriptDialog,
    exportDialog, confirmDialog, deleteConfirm,
} from './dialogs.js';

export function mountEditor(root, prefs) {
    const app = document.createElement('div');
    app.id = 'app';
    app.innerHTML = `
        <div id="menubar"></div>
        <div id="toolbar"></div>
        <div class="panel" id="hierarchy">
            <div class="panel-header">${t('panel.hierarchy')}<span class="spacer"></span></div>
        </div>
        <div class="vsplit" id="split-v"></div>
        <div id="center"></div>
        <div class="hsplit" id="split-h"></div>
        <div class="panel" id="inspector">
            <div class="panel-header">${t('panel.inspector')}</div>
        </div>
        <div class="panel" id="bottom">
            <div class="tab-strip" id="bottom-tabs"></div>
            <div style="flex:1; min-height:0; display:flex; flex-direction:column;">
                <div class="tab-page" id="page-content"></div>
                <div class="tab-page" id="page-console"></div>
            </div>
        </div>
        <div id="statusbar"></div>`;
    root.appendChild(app);

    // ---- center tabs: Scene (viewport) home --------------------------------
    const tabHost = new TabHost(document.getElementById('center'));

    // ---- panels -------------------------------------------------------------
    const hierarchy = new Hierarchy(document.getElementById('hierarchy'));
    const inspector = new Inspector(document.getElementById('inspector'));
    const consolePanel = new ConsolePanel(document.getElementById('page-console'));
    const statusbar = new StatusBar(document.getElementById('statusbar'));

    const viewportPage = tabHost.addPage('scene', {
        label: `${t('panel.scene')} — ${sceneName()}`,
        icon: 'scene',
        closeable: false,
    });
    const viewport = new Viewport(viewportPage.pageEl);

    const contentBrowser = new ContentBrowser(document.getElementById('page-content'), absPath => {
        const ext = path.extname(absPath).toLowerCase();
        if (['.png', '.jpg', '.jpeg', '.bmp'].includes(ext)) openImageTab(absPath);
        else openCodeTab(absPath);
    });
    document.getElementById('bottom')['__browser'] = contentBrowser;

    // Bottom dock tabs (simple custom strip).
    const bottomPages = { content: document.getElementById('page-content'), console: document.getElementById('page-console') };
    const bottomStrip = document.getElementById('bottom-tabs');
    function activateBottom(which) {
        for (const [id, el] of Object.entries(bottomPages)) el.classList.toggle('active', id === which);
        editor.activeBottom = which;
        renderBottomTabs();
    }
    function renderBottomTabs() {
        bottomStrip.innerHTML = '';
        for (const [id, page] of Object.entries(bottomPages)) {
            const tab = document.createElement('div');
            tab.className = 'tab' + (editor.activeBottom === id ? ' active' : '');
            tab.innerHTML = `${icon(id === 'console' ? 'console' : 'folder', 12)}<span>${t(id === 'console' ? 'panel.console' : 'panel.content')}</span>`;
            tab.addEventListener('click', () => activateBottom(id));
            bottomStrip.appendChild(tab);
        }
    }
    activateBottom('content');

    // ---- toolbar & menubar ---------------------------------------------------
    let buildRunning = false;
    const toolbar = new Toolbar(document.getElementById('toolbar'), viewport, {
        playControl: which => playControl(which),
    });

    const status = msg => statusbar.setMessage(msg);

    async function buildScripts(rebuild) {
        if (buildRunning) return;
        if (!editor.project) return;
        buildRunning = true;
        status(t('status.compiling'));
        const r = await ipc.buildScripts(editor.project.path);
        buildRunning = false;
        if (r.noSources) { status(t('status.noSources')); return; }
        if (r.ok) {
            status(t('status.buildOk', { ms: r.durationMs }));
            const loaded = engineInstance().loadScriptLibrary(r.output);
            if (loaded.ok) setScriptClasses(loaded.classes);
            for (const [, page] of tabHost.pages) {
                if (page.codeEditor) page.codeEditor.applyDiagnostics([]);
            }
        } else {
            status(t('status.buildFailed'));
            for (const [, page] of tabHost.pages) {
                if (page.codeEditor) page.codeEditor.applyDiagnostics(r.diagnostics);
            }
            consolePanel.entries.push(...r.diagnostics
                .filter(d => d.severity === 'error')
                .map(d => ({ level: 'error', text: `${d.file}:${d.line}: ${d.message}`, time: new Date() })));
            consolePanel.render();
        }
    }

    function playControl(which) {
        const eng = engineInstance();
        if (which === 'play' && !editor.playing) {
            if (prefs.autosave && editor.sceneDirty) saveScene();
            eng.play();
            editor.playing = true;
            editor.paused = false;
            viewport.focusBadge.style.display = 'block';
            viewport.focusBadge.textContent = t('status.playerFocus');
        } else if (which === 'pause') {
            if (editor.playing) {
                if (editor.paused) eng.resume();
                else eng.pause();
                editor.paused = !editor.paused;
            }
        } else if (which === 'step') {
            eng.step();
        } else if (which === 'stop') {
            eng.stop();
            editor.playing = false;
            editor.paused = false;
            viewport.focusBadge.style.display = 'none';
            emit('scene-changed');
        }
        emit('play-state');
    }

    // ---- tabs: code / image editors ------------------------------------------
    async function openCodeTab(filePath) {
        const id = 'code:' + filePath;
        tabHost.addPage(id, {
            label: t('code.tab', { name: path.basename(filePath) }),
            icon: 'script',
            build: (pageEl, page) => {
                // Construct after the tab becomes visible so Monaco measures a
                // real container size.
                setTimeout(() => {
                    if (!tabHost.pages.has(id)) return;
                    import('../codeeditor.js').then(({ CodeEditor }) => {
                        if (!tabHost.pages.has(id)) return;
                        const ed = new CodeEditor(pageEl, filePath, {
                            onDirty: () => { tabHost.renderStrip(); },
                        });
                        page.codeEditor = ed;
                        requestAnimationFrame(() => ed.editor.layout());
                    });
                }, 0);
            },
            onClose: () => {
                const page = tabHost.pages.get(id);
                if (page?.codeEditor?.dirty) {
                    page.codeEditor.save();
                }
                page?.codeEditor?.dispose();
                return true;
            },
        });
    }

    function openImageTab(filePath) {
        const id = 'image:' + filePath;
        tabHost.addPage(id, {
            label: path.basename(filePath),
            icon: 'image',
            build: pageEl => {
                import('../imageeditor.js').then(({ ImageEditor }) => {
                    new ImageEditor(pageEl, filePath);
                });
            },
        });
    }

    on('open-code', absPath => openCodeTab(absPath));
    on('open-image', absPath => openImageTab(absPath));
    on('file-saved', async filePath => {
        if (/\.cpp$|\.h$|\.hpp$/i.test(filePath) && prefs.autosave !== false) {
            // Recompile scripts in the background after each script save.
            if (!buildRunning && editor.playing) return;
            buildScripts();
        }
        if (/\.(png|jpg|jpeg|bmp)$/i.test(filePath)) {
            engineInstance().reloadAssets();
        }
    });
    on('asset-changed', () => engineInstance().reloadAssets());
    on('open-scene', async absPath => {
        if (await openScene(absPath)) {
            status(t('status.sceneLoaded'));
            tabHost.activate('scene');
            tabHost.pages.get('scene').label = `${t('panel.scene')} — ${sceneName()}`;
            tabHost.renderStrip();
        }
    });

    // ---- menus -----------------------------------------------------------------
    const menus = [
        {
            label: 'menu.file',
            items: [
                { label: 'menu.file.newScene', shortcut: 'Ctrl+N', run: () => newSceneDialog(name => { newScene(name); status(t('status.sceneCreated', { name })); }) },
                { label: 'menu.file.openScene', shortcut: 'Ctrl+O', run: () => openSceneDialog() },
                { label: 'menu.file.saveScene', shortcut: 'Ctrl+S', run: () => saveScene().then(ok => ok && status(t('status.sceneSaved'))) },
                '-',
                { label: 'menu.file.newScript', shortcut: 'Ctrl+Shift+N', run: () => newScriptDialog(editor.project, name => {
                    const file = path.join(editor.project.path, 'Assets', 'Scripts', name + '.cpp');
                    fs.writeFileSync(file, scriptTemplate(name));
                    openCodeTab(file);
                    contentBrowser.refresh();
                    status(t('status.scriptCreated', { name }));
                }) },
                '-',
                { label: 'menu.file.preferences', run: async () => {
                    settingsDialog(await ipc.getPrefs(), async patch => {
                        await ipc.setPrefs(patch);
                        if (patch.language && patch.language !== language()) setLanguage(patch.language);
                        if (patch.showStats !== undefined) viewport.showStats = patch.showStats;
                        if (patch.compiler !== undefined) detectCompilerStatus();
                    });
                } },
                '-',
                ...(isMac() ? [] : [{ label: 'menu.file.exit', run: () => window.close() }]),
            ],
        },
        {
            label: 'menu.edit',
            items: [
                { label: 'menu.edit.undo', shortcut: 'Ctrl+Z', run: undo },
                { label: 'menu.edit.redo', shortcut: 'Ctrl+Y', run: redo },
                '-',
                { label: 'menu.edit.duplicate', shortcut: 'Ctrl+D', enabled: () => editor.selection, run: () => duplicateEntity(editor.selection) },
                { label: 'menu.edit.delete', shortcut: 'Del', enabled: () => editor.selection, run: async () => {
                    const ent = entityList().find(e => e.id === editor.selection);
                    if (ent && await deleteConfirm('entity', ent.name)) destroyEntity(ent.id);
                } },
            ],
        },
        {
            label: 'menu.assets',
            items: [
                { label: 'menu.assets.import', run: () => activateBottom('content') },
                { label: 'menu.assets.refresh', run: () => contentBrowser.refresh() },
                '-',
                { label: 'menu.assets.showInFolder', run: () => ipc.showItemInFolder(editor.project.path) },
            ],
        },
        {
            label: 'menu.build',
            items: [
                { label: 'menu.build.scripts', shortcut: 'Ctrl+B', run: () => buildScripts() },
                { label: 'menu.build.rebuild', run: () => buildScripts(true) },
                '-',
                { label: 'menu.build.exportPlayer', run: () => exportDialog(editor.project, async dir => {
                    const r = await ipc.exportPlayer(editor.project.path, dir);
                    if (r.ok) status(t('status.exportOk', { dir: r.dir }));
                    else status(t('status.exportFailed', { error: r.error }));
                    return r;
                }) },
            ],
        },
        {
            label: 'menu.window',
            items: [
                { label: 'menu.window.content', run: () => activateBottom('content') },
                { label: 'menu.window.console', run: () => activateBottom('console') },
                { label: 'menu.window.stats', run: () => { viewport.showStats = !viewport.showStats; } },
            ],
        },
        {
            label: 'menu.help',
            items: [
                { label: 'menu.help.docs', run: () => ipc.openExternal('https://github.com/cairn-engine/cairn-engine/tree/main/docs') },
                { label: 'menu.help.scriptingApi', run: () => ipc.openExternal('https://github.com/cairn-engine/cairn-engine/blob/main/docs/scripting-api.md') },
                '-',
                { label: 'menu.help.about', run: async () => aboutDialog(await ipc.appVersion()) },
            ],
        },
    ];
    new MenuBar(document.getElementById('menubar'), menus);

    async function openSceneDialog() {
        const file = await ipc.pickOpenFile({
            title: t('menu.file.openScene'),
            filters: [{ name: 'Cairn Scene', extensions: ['cscene'] }],
            defaultPath: path.join(editor.project.path, 'Assets', 'Scenes'),
        });
        if (file) emit('open-scene', file);
    }

    async function detectCompilerStatus() {
        const compiler = await ipc.detectCompiler();
        if (!compiler) status(t('status.noCompiler'));
        else status(`${t('status.ready')} — ${compiler}`);
        return compiler;
    }
    detectCompilerStatus();

    // ---- layout splitters --------------------------------------------------------
    attachSplitter(document.getElementById('split-v'), 'col', panel => {
        panel.style.width = '';
    }, document.getElementById('hierarchy'));
    attachSplitter(document.getElementById('split-h'), 'col', () => {}, document.getElementById('inspector'));

    function attachSplitter(el, axis, _, panel) {
        el.addEventListener('mousedown', ev => {
            ev.preventDefault();
            const startX = ev.clientX, startY = ev.clientY;
            const rect = panel.getBoundingClientRect();
            const move = me => {
                const delta = axis === 'col' ? me.clientX - startX : me.clientY - startY;
                panel.style.width = Math.max(160, rect.width - delta) + 'px';
            };
            const up = () => {
                window.removeEventListener('mousemove', move);
                window.removeEventListener('mouseup', up);
                viewport.resize();
            };
            window.addEventListener('mousemove', move);
            window.addEventListener('mouseup', up);
        });
    }

    // ---- keyboard shortcuts --------------------------------------------------------
    window.addEventListener('keydown', ev => {
        const ctrl = ev.ctrlKey || ev.metaKey;
        const inInput = /INPUT|TEXTAREA|SELECT/.test(document.activeElement?.tagName) ||
            document.activeElement?.closest('.monaco-editor');
        if (inInput && !ctrl) return;
        if (ctrl && ev.key.toLowerCase() === 's') { ev.preventDefault(); saveScene().then(ok => ok && status(t('status.sceneSaved'))); return; }
        if (ctrl && ev.key.toLowerCase() === 'b') { ev.preventDefault(); buildScripts(); return; }
        if (ctrl && ev.key.toLowerCase() === 'p') { ev.preventDefault(); playControl(editor.playing ? 'stop' : 'play'); return; }
        if (ctrl && ev.key.toLowerCase() === 'z') { ev.preventDefault(); undo(); return; }
        if (ctrl && ev.key.toLowerCase() === 'y') { ev.preventDefault(); redo(); return; }
        if (ctrl && ev.key.toLowerCase() === 'd') { ev.preventDefault(); if (editor.selection) duplicateEntity(editor.selection); return; }
        if (ctrl && ev.key.toLowerCase() === 'o') { ev.preventDefault(); openSceneDialog(); return; }
        if (ctrl && ev.key.toLowerCase() === 'n') { ev.preventDefault(); newSceneDialog(name => newScene(name)); return; }
        if (ev.key === 'Delete' && editor.selection && !inInput) {
            const ent = entityList().find(e => e.id === editor.selection);
            if (ent) destroyEntity(ent.id);
            return;
        }
        if (ev.key === 'F' && !inInput) { viewport.focusSelected(); return; }
        if (!inInput) {
            const toolKeys = { q: 'select', w: 'move', e: 'rotate', r: 'scale' };
            if (toolKeys[ev.key.toLowerCase()] && !editor.playing) {
                toolbar.setTool(toolKeys[ev.key.toLowerCase()]);
            }
        }
    });

    // ---- frame loop -----------------------------------------------------------------
    on('scene-changed', () => {
        tabHost.pages.get('scene').label = `${t('panel.scene')} — ${sceneName()}`;
        tabHost.renderStrip();
        statusbar.update({
            sceneLabel: `${sceneName()}.cscene`,
            dirty: editor.sceneDirty,
            playing: editor.playing,
            paused: editor.paused,
            fps: lastFps,
            stats: lastStats,
            selection: entityList().find(e => e.id === editor.selection) || null,
        });
    });
    on('scene-dirty', () => {
        statusbar.update({
            sceneLabel: `${sceneName()}.cscene`,
            dirty: true, playing: editor.playing, paused: editor.paused,
            fps: lastFps, stats: lastStats,
            selection: entityList().find(e => e.id === editor.selection) || null,
        });
    });

    let lastFps = 60;
    let lastStats = { drawCalls: 0, triangles: 0 };
    let last = performance.now();
    let fpsAccum = 0, fpsFrames = 0;

    // Introspection + automation hooks (tests and the autotest harness).
    window.__cairnDebug = () => ({
        canvas: [viewport.sceneCanvas.width, viewport.sceneCanvas.height],
        stats: viewport.lastRenderStats || null,
        entities: entityList().length,
        playing: editor.playing,
    });
    window.__cairnApp = {
        openCode: abs => openCodeTab(abs),
        openImage: abs => openImageTab(abs),
        build: () => buildScripts(),
        play: which => playControl(which || 'play'),
        selectFirst: () => {
            const list = entityList();
            if (list.length) selectEntity(list[Math.min(3, list.length - 1)].id);
        },
        tabs: tabHost,
        focusScene: () => tabHost.activate('scene'),
    };

    function frame(now) {
        const dt = Math.min(0.1, (now - last) / 1000);
        last = now;
        fpsAccum += dt; fpsFrames++;
        if (fpsAccum >= 0.5) {
            lastFps = fpsFrames / fpsAccum;
            fpsAccum = 0; fpsFrames = 0;
        }

        viewport.frame(dt);
        if (editor.playing && !editor.paused) engineInstance().update(dt);
        if (editor.playing) consolePanel.pump();
        lastStats = viewport.lastRenderStats || lastStats;

        statusbar.update({
            sceneLabel: `${sceneName()}.cscene`,
            dirty: editor.sceneDirty,
            playing: editor.playing,
            paused: editor.paused,
            fps: lastFps,
            stats: lastStats,
            selection: entityList().find(e => e.id === editor.selection) || null,
        });
        requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);

    return { viewport, tabHost, buildScripts, playControl };
}

function isMac() { return process.platform === 'darwin'; }
