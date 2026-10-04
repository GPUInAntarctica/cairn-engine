// Cairn Engine player - main process for exported games.
'use strict';

const { app, BrowserWindow } = require('electron');
const path = require('path');

let win = null;

function createWindow() {
    win = new BrowserWindow({
        width: 1280,
        height: 720,
        minWidth: 640,
        minHeight: 400,
        backgroundColor: '#000000',
        title: 'Cairn Player',
        useContentSize: true,
        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false,
            sandbox: false,
        },
    });
    win.setMenu(null);
    win.loadFile(path.join(__dirname, 'index.html'));
    win.on('closed', () => { win = null; });
}

app.whenReady().then(() => {
    createWindow();
    app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});

app.on('window-all-closed', () => app.quit());
