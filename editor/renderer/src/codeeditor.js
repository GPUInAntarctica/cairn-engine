// Cairn Engine editor - Monaco-based C++ code editor with engine API
// completion and build diagnostics. Monaco is imported through its ESM
// entry so the CommonJS bundle resolves it cleanly; the web worker ships as
// a separate bundle entry (scripts/build.js).
'use strict';

import path from 'path';
import fs from 'fs';

import * as monaco from 'monaco-editor/esm/vs/editor/editor.api.js';
import 'monaco-editor/esm/vs/editor/editor.all.js';

import { editor, scriptClassMeta } from './state.js';
import { emit, on } from './events.js';
import { t } from './i18n.js';

function appRoot() {
    const base = window.location.href.replace(/[?#].*$/, '').replace(/\/index\.html$/, '');
    return path.dirname(path.dirname(decodeURIComponent(base.replace(/^file:\/\//, ''))));
}

const API_JSON = path.join(appRoot(), 'editor', 'renderer', 'src', 'engine-api.json');

let themeReady = false;
function ensureTheme() {
    if (themeReady) return;
    themeReady = true;
    monaco.editor.defineTheme('cairn-dark', {
        base: 'vs-dark',
        inherit: true,
        rules: [
            { token: 'comment', foreground: '5c5c66', fontStyle: 'italic' },
            { token: 'keyword', foreground: '6d9ee8' },
            { token: 'keyword.directive', foreground: '8b8b97' },
            { token: 'number', foreground: 'c8a04a' },
            { token: 'string', foreground: '8fbf7f' },
            { token: 'type', foreground: '7fc8c8' },
            { token: 'identifier', foreground: 'c9c9d2' },
        ],
        colors: {
            'editor.background': '#1c1c23',
            'editor.lineHighlightBackground': '#22222b',
            'editorLineNumber.foreground': '#4a4a56',
            'editorLineNumber.activeForeground': '#8b8b97',
            'editorGutter.background': '#1c1c23',
            'editor.selectionBackground': '#2b4a6f',
            'editorIndentGuide.background1': '#2a2a34',
        },
    });
}

let completionReady = false;
function ensureCompletion() {
    if (completionReady) return;
    completionReady = true;
    let api = [];
    try { api = JSON.parse(fs.readFileSync(API_JSON, 'utf8')); } catch { /* optional */ }
    const kinds = {
        class: monaco.languages.CompletionItemKind.Class,
        method: monaco.languages.CompletionItemKind.Method,
        field: monaco.languages.CompletionItemKind.Field,
    };
    monaco.languages.registerCompletionItemProvider('cpp', {
        triggerCharacters: ['.'],
        provideCompletionItems: (model, position) => {
            const word = model.getWordUntilPosition(position);
            const range = {
                startLineNumber: position.lineNumber, endLineNumber: position.lineNumber,
                startColumn: word.startColumn, endColumn: word.endColumn,
            };
            const items = editor.scriptClasses.map(c => ({
                kind: kinds.class, label: c.name, insertText: c.name,
                detail: 'gameplay script', range,
            }));
            for (const entry of api) {
                items.push({
                    kind: kinds[entry.kind] || kinds.field,
                    label: entry.label,
                    insertText: entry.insert || entry.label,
                    detail: entry.detail || '',
                    range,
                });
            }
            return { suggestions: items };
        },
    });
}

export class CodeEditor {
    constructor(pageEl, filePath, { onDirty }) {
        this.filePath = filePath;
        this.pageEl = pageEl;
        this.onDirty = onDirty;
        this.dirty = false;
        pageEl.innerHTML = `<div class="code-host"></div>`;
        this.host = pageEl.querySelector('.code-host');

        ensureTheme();
        ensureCompletion();
        this.editor = monaco.editor.create(this.host, {
            value: fs.readFileSync(this.filePath, 'utf8'),
            language: 'cpp',
            theme: 'cairn-dark',
            fontSize: 13,
            fontFamily: 'Cascadia Mono, Consolas, monospace',
            minimap: { enabled: true, scale: 1 },
            renderWhitespace: 'selection',
            scrollBeyondLastLine: false,
            automaticLayout: true,
            tabSize: 4,
            rulers: [100],
        });
        this.model = this.editor.getModel();
        this.model.updateOptions({ tabSize: 4, insertSpaces: true });
        this.editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => this.save());
        this.model.onDidChangeContent(() => {
            if (!this.dirty) {
                this.dirty = true;
                this.onDirty(this);
            }
        });
    }

    save() {
        fs.writeFileSync(this.filePath, this.model.getValue());
        this.dirty = false;
        this.onDirty(this);
        emit('file-saved', this.filePath);
        return true;
    }

    applyDiagnostics(diagnostics) {
        const markers = diagnostics
            .filter(d => path.resolve(d.file) === path.resolve(this.filePath))
            .map(d => ({
                severity: d.severity === 'warning' ? monaco.MarkerSeverity.Warning : monaco.MarkerSeverity.Error,
                startLineNumber: d.line || 1,
                startColumn: d.column || 1,
                endLineNumber: d.line || 1,
                endColumn: (d.column || 1) + 4,
                message: d.message,
            }));
        monaco.editor.setModelMarkers(this.model, 'build', markers);
    }

    dispose() {
        this.editor.dispose();
    }
}
