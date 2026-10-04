// Cairn Engine editor - preferences store (main process).
// Persisted under the user's home directory so projects stay portable.
'use strict';

const fs = require('fs');
const path = require('path');

const dir = process.env.CAIRN_HOME || path.join(process.env.HOME || process.env.USERPROFILE || '.', '.cairn-engine');
const file = path.join(dir, 'preferences.json');

const defaults = () => ({
    language: null,               // null = system default
    compiler: '',                 // explicit compiler path override
    recentProjects: [],           // [{path, name, lastOpened}]
    autosave: true,
    showStats: true,
    lastProject: '',
});

function load() {
    try {
        const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
        return { ...defaults(), ...raw };
    } catch {
        return defaults();
    }
}

function save(prefs) {
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(file, JSON.stringify(prefs, null, 2));
    return prefs;
}

function touchRecent(prefs, projectPath, projectName) {
    const list = (prefs.recentProjects || []).filter(p => p.path !== projectPath);
    list.unshift({ path: projectPath, name: projectName, lastOpened: Date.now() });
    prefs.recentProjects = list.slice(0, 10);
    prefs.lastProject = projectPath;
    return save(prefs);
}

module.exports = { load, save, touchRecent, file };
