// Cairn Engine editor - main process.
'use strict';

const { app, BrowserWindow, ipcMain, dialog, shell, Menu } = require('electron');
const path = require('path');
const fs = require('fs');

const preferences = require('./preferences');
const project = require('./project');
const buildPipeline = require('./build');
const exporter = require('./export');

const isMac = process.platform === 'darwin';
let mainWindow = null;

// Renderer processes get nodeIntegration (the editor is a local tool that
// owns its files), so IPC only carries operations that need native dialogs,
// compilation and the shell.
function registerIpc() {
    ipcMain.handle('prefs:get', () => preferences.load());
    ipcMain.handle('prefs:set', (_e, patch) => preferences.save({ ...preferences.load(), ...patch }));

    ipcMain.handle('dialog:openDirectory', async (_e, opts) => {
        const r = await dialog.showOpenDialog(mainWindow, { properties: ['openDirectory'], ...(opts || {}) });
        return r.canceled ? null : r.filePaths[0];
    });
    ipcMain.handle('dialog:openFile', async (_e, opts) => {
        const r = await dialog.showOpenDialog(mainWindow, opts || {});
        return r.canceled ? null : r.filePaths[0];
    });
    ipcMain.handle('dialog:saveFile', async (_e, opts) => {
        const r = await dialog.showSaveDialog(mainWindow, opts || {});
        return r.canceled ? null : r.filePath;
    });
    ipcMain.handle('dialog:message', async (_e, opts) => {
        const r = await dialog.showMessageBox(mainWindow, opts || {});
        return r.response;
    });

    ipcMain.handle('project:listTemplates', () => project.listTemplates());
    ipcMain.handle('project:read', (_e, dir) => project.readProject(dir));
    ipcMain.handle('project:create', (_e, options) => project.createProject(options, preferences.load()));
    ipcMain.handle('project:recent', () => {
        const prefs = preferences.load();
        return (prefs.recentProjects || []).filter(p => fs.existsSync(p.path));
    });

    ipcMain.handle('build:scripts', async (_e, projectDir) => {
        const prefs = preferences.load();
        const compiler = prefs.compiler || 'g++';
        const result = await buildPipeline.buildScripts(projectDir, compiler);
        return result;
    });
    ipcMain.handle('build:detectCompiler', async () => {
        const prefs = preferences.load();
        const found = await buildPipeline.detectCompiler(prefs.compiler);
        return found || '';
    });

    ipcMain.handle('export:player', async (_e, { projectDir, targetDir }) => {
        const prefs = preferences.load();
        return exporter.exportPlayer(projectDir, targetDir, buildPipeline.buildScripts, prefs.compiler || 'g++');
    });

    ipcMain.handle('shell:showItemInFolder', (_e, full) => shell.showItemInFolder(full));
    ipcMain.handle('shell:openExternal', (_e, url) => shell.openExternal(url));
    ipcMain.handle('shell:openPath', (_e, p) => shell.openPath(p));
    ipcMain.handle('app:version', () => app.getVersion());
}

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 1500,
        height: 940,
        minWidth: 1150,
        minHeight: 700,
        backgroundColor: '#141419',
        title: 'Cairn Engine',
        show: false,
        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false,
            sandbox: false,
            spellcheck: false,
        },
    });

    const target = process.argv.includes('--project-browser') || !process.argv.some(a => a.startsWith('--project='))
        ? 'renderer/index.html'
        : 'renderer/index.html';
    mainWindow.loadFile(path.join(__dirname, '..', target));

    mainWindow.once('ready-to-show', () => mainWindow.show());
    mainWindow.on('closed', () => { mainWindow = null; });

    // Suppress default menu; the editor draws its own (localizable) bar.
    Menu.setApplicationMenu(null);
}

app.whenReady().then(() => {
    registerIpc();
    createWindow();

    // Headless smoke support: CAIRN_AUTOTEST=1 quits after the UI settles and
    // reports renderer console errors through the exit code.
    if (process.env.CAIRN_AUTOTEST) {
        mainWindow.webContents.on('console-message', (_e, level, message) => {
            if (level >= 2) console.error(`[renderer-error] ${message}`);
        });
        setTimeout(async () => {
            try {
                // Scripted UI actions: CAIRN_AUTOTEST_ACTIONS="select,code,image,play".
                const actions = (process.env.CAIRN_AUTOTEST_ACTIONS || '').split(',').filter(Boolean);
                await mainWindow.webContents.executeJavaScript(
                    `window.OPEN_TARGET = {
                        code: ${JSON.stringify(process.env.CAIRN_AUTOTEST_SCRIPT || '')},
                        image: ${JSON.stringify(process.env.CAIRN_AUTOTEST_TEXTURE || '')},
                    }`, true);
                const shotDir = process.env.CAIRN_AUTOTEST_SHOT_DIR || '';
                for (const action of actions) {
                    const hooks = {
                        select: 'window.__cairnApp.selectFirst()',
                        scene: 'window.__cairnApp.focusScene()',
                        code: 'window.__cairnApp.openCode(window.OPEN_TARGET.code)',
                        image: 'window.__cairnApp.openImage(window.OPEN_TARGET.image)',
                        play: "window.__cairnApp.play('play')",
                        stop: "window.__cairnApp.play('stop')",
                    };
                    const call = hooks[action] || `window.__cairnApp.${action}()`;
                    await mainWindow.webContents.executeJavaScript(call, true);
                    await new Promise(r => setTimeout(r, action === 'play' ? 2600 : action === 'code' ? 4500 : 2000));
                    if (shotDir) {
                        const image = await mainWindow.webContents.capturePage();
                        const file = path.join(shotDir, `stage-${action}.png`);
                        fs.writeFileSync(file, image.toPNG());
                        console.log(`[autotest] shot ${file}`);
                    }
                }
                if (process.env.CAIRN_AUTOTEST_DOM) {
                    const probeJs = [
                        'JSON.stringify((function(){',
                        "  var host = document.querySelector('.code-host');",
                        '  var r = host ? host.getBoundingClientRect() : null;',
                        '  var el = document.elementFromPoint(1110, 715);',
                        '  return {',
                        '    codeHostRect: r ? { x: r.x, y: r.y, w: r.width, h: r.height } : null,',
                        '    atGarbled: el ? (el.className || el.tagName) : null,',
                        "    monacoLines: document.querySelectorAll('.monaco-editor .view-line').length,",
                        '    debug: window.__cairnDebug ? window.__cairnDebug() : null',
                        '  };',
                        '})())',
                    ].join('\n');
                    const info = await mainWindow.webContents.executeJavaScript(probeJs);
                    console.log(`[autotest-dom] ${info}`);
                }
                if (process.env.CAIRN_AUTOTEST_SHOT) {
                    const image = await mainWindow.webContents.capturePage();
                    fs.writeFileSync(process.env.CAIRN_AUTOTEST_SHOT, image.toPNG());
                    console.log(`[autotest] screenshot -> ${process.env.CAIRN_AUTOTEST_SHOT}`);
                }
            } catch (err) {
                console.error(`[autotest] capture failed: ${err.message}`);
            }
            console.log('[autotest] window settled, quitting');
            app.exit(0);
        }, Number(process.env.CAIRN_AUTOTEST_MS) || 6000);
    }

    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => {
    if (!isMac) app.quit();
});
