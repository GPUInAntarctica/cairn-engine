// Cairn Engine editor - editor state and engine binding.
// Single source of truth for the project, selection, undo history and the
// frame loop that drives the native engine.
'use strict';

import path from 'path';
import fs from 'fs';
import { emit } from './events.js';
import { DEFAULT_SUN_ROTATION } from './scene_defaults.mjs';

// Root of the application, derived from the loaded page so the same code
// works from the source tree and the bundled renderer (dist/).
function appRoot() {
    const base = window.location.href.replace(/[?#].*$/, '').replace(/\/index\.html$/, '');
    const local = decodeURIComponent(base.replace(/^file:\/\//, ''));
    // <root>/editor/renderer -> editor -> <root>
    return path.dirname(path.dirname(local));
}

const ADDON_PATH = path.join(appRoot(), 'build', 'cairn_engine_addon.node');

let addon = null;
let engine = null;

export const editor = {
    project: null,          // {path, name, template}
    scenePath: '',          // absolute path of the open scene ('' = untitled)
    sceneDirty: false,
    selection: 0,           // entity id
    tool: 'move',           // select | move | rotate | scale
    playing: false,
    paused: false,
    showStats: true,
    scriptClasses: [],      // [{name, props:[{name,type,default}]}]
    undoStack: [],
    redoStack: [],
    clipboard: null,        // entity subtree JSON (string)
    openTabs: [],           // center tabs managed by tabs.js
    activeBottom: 'content',
};

export function loadAddon() {
    if (!addon) addon = require(ADDON_PATH);
    return addon;
}

export function engineInstance() {
    if (!engine) {
        engine = new (loadAddon().Engine)();
        engine.setViewportSize(960, 540);
    }
    return engine;
}

export function engineVersion() {
    try { return loadAddon().engineVersion; } catch { return '?'; }
}

// ---------------------------------------------------------------------------
// Project / scene lifecycle.
// ---------------------------------------------------------------------------
export function openProject(projectDoc) {
    editor.project = projectDoc;
    engineInstance().setProjectRoot(projectDoc.path);
    // Restore the last opened scene if it still exists.
    const scenesDir = path.join(projectDoc.path, 'Assets', 'Scenes');
    let scene = '';
    try {
        const found = fs.readdirSync(scenesDir).filter(f => f.endsWith('.cscene')).sort();
        if (found.length) scene = path.join(scenesDir, found[0]);
    } catch { /* ignore */ }
    if (scene) {
        const r = engineInstance().loadScene(scene);
        if (r.ok) {
            editor.scenePath = scene;
            editor.sceneDirty = false;
        }
    }
    emit('project-opened');
    newSceneIfNeeded();
}

function newSceneIfNeeded() {
    if (entityList().length === 0) {
        createDefaults();
    }
    emit('scene-changed');
}

export function createDefaults() {
    // Light rig + main camera, the baseline every engine gives you.
    const sun = engineInstance().createEntity('Directional Light', 0);
    engineInstance().addComponent(sun, 'Light');
    engineInstance().setComponentProp(sun, 'Transform', 'rotation', [...DEFAULT_SUN_ROTATION]);
    engineInstance().setComponentProp(sun, 'Light', 'intensity', 1.0);

    const cam = engineInstance().createEntity('Main Camera', 0);
    engineInstance().addComponent(cam, 'Camera');
    engineInstance().setEntityTag(cam, 'MainCamera');
    engineInstance().setComponentProp(cam, 'Transform', 'position', [0, 2, 8]);
    engineInstance().setComponentProp(cam, 'Transform', 'rotation', [-8, 0, 0]);
    editor.sceneDirty = true;
}

export function sceneName() {
    if (!editor.scenePath) return 'Untitled';
    return path.basename(editor.scenePath, '.cscene');
}

export function markDirty() {
    editor.sceneDirty = true;
    emit('scene-dirty');
}

// ---------------------------------------------------------------------------
// Undo / redo via whole-scene snapshots (cheap at editor scene sizes).
// ---------------------------------------------------------------------------
export function pushUndo() {
    editor.undoStack.push(engineInstance().sceneJSON());
    if (editor.undoStack.length > 100) editor.undoStack.shift();
    editor.redoStack.length = 0;
}

export function undo() {
    if (!editor.undoStack.length) return false;
    editor.redoStack.push(engineInstance().sceneJSON());
    engineInstance().restoreSceneJSON(editor.undoStack.pop());
    clampSelection();
    editor.sceneDirty = true;
    emit('scene-changed');
    return true;
}

export function redo() {
    if (!editor.redoStack.length) return false;
    editor.undoStack.push(engineInstance().sceneJSON());
    engineInstance().restoreSceneJSON(editor.redoStack.pop());
    clampSelection();
    editor.sceneDirty = true;
    emit('scene-changed');
    return true;
}

function clampSelection() {
    const ids = entityList().map(e => e.id);
    if (!ids.includes(editor.selection)) editor.selection = 0;
}

// ---------------------------------------------------------------------------
// Entity helpers (thin, undo-aware wrappers over the addon).
// ---------------------------------------------------------------------------
export function entityList() {
    try {
        return engineInstance().getSceneTree().entities;
    } catch {
        return [];
    }
}

export function selectedEntity() {
    if (!editor.selection) return null;
    return entityList().find(e => e.id === editor.selection) || null;
}

export function selectEntity(id) {
    editor.selection = id | 0;
    engineInstance().setHighlightEntity(editor.selection);
    emit('selection-changed');
}

export function createEntity(kind, name) {
    pushUndo();
    const eng = engineInstance();
    let id = 0;
    switch (kind) {
        case 'empty': id = eng.createEntity(name || 'GameObject', 0); break;
        case 'sprite': id = eng.createSprite(name || 'Sprite', 0); break;
        case 'camera': {
            id = eng.createEntity(name || 'Camera', 0);
            eng.addComponent(id, 'Camera');
            eng.setComponentProp(id, 'Transform', 'position', [0, 2, 8]);
            break;
        }
        case 'light-directional': {
            id = eng.createEntity(name || 'Directional Light', 0);
            eng.addComponent(id, 'Light');
            eng.setComponentProp(id, 'Transform', 'rotation', [...DEFAULT_SUN_ROTATION]);
            break;
        }
        case 'light-point': {
            id = eng.createEntity(name || 'Point Light', 0);
            eng.addComponent(id, 'Light');
            eng.setComponentProp(id, 'Light', 'type', 1);
            eng.setComponentProp(id, 'Transform', 'position', [0, 3, 0]);
            break;
        }
        default: {  // primitive mesh names
            id = eng.createPrimitive(kind, name || kind[0].toUpperCase() + kind.slice(1), 0);
        }
    }
    selectEntity(id);
    markDirty();
    return id;
}

export function destroyEntity(id) {
    pushUndo();
    engineInstance().destroyEntity(id);
    if (editor.selection === id) selectEntity(0);
    markDirty();
}

export function duplicateEntity(id) {
    pushUndo();
    const newId = engineInstance().duplicateEntity(id);
    if (newId) selectEntity(newId);
    markDirty();
    return newId;
}

// ---------------------------------------------------------------------------
// Save / load.
// ---------------------------------------------------------------------------
export async function saveScene() {
    if (!editor.scenePath) {
        const { pickSaveFile } = await import('./ipc.js');
        const target = await pickSaveFile({
            title: 'Save Scene',
            filters: [{ name: 'Cairn Scene', extensions: ['cscene'] }],
            defaultPath: path.join(editor.project.path, 'Assets', 'Scenes', 'Main.cscene'),
        });
        if (!target) return false;
        editor.scenePath = target.endsWith('.cscene') ? target : target + '.cscene';
    }
    const r = engineInstance().saveScene(editor.scenePath);
    if (r.ok) {
        editor.sceneDirty = false;
        emit('scene-saved');
        return true;
    }
    return false;
}

export async function openScene(filePath) {
    if (editor.sceneDirty) {
        const { confirmDialog } = await import('./ui/dialogs.js');
        const choice = await confirmDialog(
            'dialog.overwriteScene',
            [
                { label: 'dialog.save', value: 'save', primary: true },
                { label: 'dialog.dontSave', value: 'discard' },
                { label: 'dialog.cancel', value: 'cancel' },
            ]);
        if (choice === 'cancel') return false;
        if (choice === 'save' && !(await saveScene())) return false;
    }
    const r = engineInstance().loadScene(filePath);
    if (!r.ok) return false;
    editor.scenePath = filePath;
    editor.sceneDirty = false;
    editor.undoStack.length = 0;
    editor.redoStack.length = 0;
    selectEntity(0);
    emit('scene-changed');
    return true;
}

export function newScene(name) {
    pushUndo();
    engineInstance().newScene(name || 'Scene');
    createDefaults();
    editor.scenePath = '';
    editor.sceneDirty = true;
    selectEntity(0);
    emit('scene-changed');
}

// ---------------------------------------------------------------------------
// Script classes (populated after a successful build).
// ---------------------------------------------------------------------------
export function setScriptClasses(classes) {
    editor.scriptClasses = classes || [];
    emit('script-classes-changed');
}

export function scriptClassMeta(name) {
    return editor.scriptClasses.find(c => c.name === name) || null;
}
