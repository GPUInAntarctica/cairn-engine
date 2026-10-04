// Cairn Engine editor - IPC bridge to the main process.
'use strict';

const { ipcRenderer } = require('electron');

export const ipc = {
    getPrefs: () => ipcRenderer.invoke('prefs:get'),
    setPrefs: patch => ipcRenderer.invoke('prefs:set', patch),

    pickDirectory: opts => ipcRenderer.invoke('dialog:openDirectory', opts),
    pickOpenFile: opts => ipcRenderer.invoke('dialog:openFile', opts),
    pickSaveFile: opts => ipcRenderer.invoke('dialog:saveFile', opts),
    messageBox: opts => ipcRenderer.invoke('dialog:message', opts),

    listTemplates: () => ipcRenderer.invoke('project:listTemplates'),
    readProject: dir => ipcRenderer.invoke('project:read', dir),
    createProject: opts => ipcRenderer.invoke('project:create', opts),
    recentProjects: () => ipcRenderer.invoke('project:recent'),

    buildScripts: projectDir => ipcRenderer.invoke('build:scripts', projectDir),
    detectCompiler: () => ipcRenderer.invoke('build:detectCompiler'),

    exportPlayer: (projectDir, targetDir) => ipcRenderer.invoke('export:player', { projectDir, targetDir }),

    showItemInFolder: p => ipcRenderer.invoke('shell:showItemInFolder', p),
    openExternal: url => ipcRenderer.invoke('shell:openExternal', url),
    openPath: p => ipcRenderer.invoke('shell:openPath', p),
    appVersion: () => ipcRenderer.invoke('app:version'),
};
