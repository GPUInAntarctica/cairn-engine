// Cairn Engine editor - Monaco-based C++ code editor with engine API
// completion and build diagnostics. Monaco is imported through its ESM
// entry so the CommonJS bundle resolves it cleanly; the web worker ships as
// a separate bundle entry (scripts/build.js).
'use strict';

import path from 'path';
import fs from 'fs';

import * as monaco from 'monaco-editor/esm/vs/editor/editor.api.js';
import 'monaco-editor/esm/vs/editor/editor.all.js';
// editor.all bundles the core editor and its features, but NOT the language
// grammars: those live in basic-languages. Without this import the cpp model
// is registered with no tokenizer, every span falls back to the default token
// colour, and the editor renders as an undifferentiated wall of light grey.
import 'monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution.js';
// editor.all only bundles the core editor; the language grammars live in
// basic-languages. Without this import the cpp model has no tokenizer, so
// every line falls back to the bare identifier colour and the editor renders
// as a wall of white.
import 'monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution.js';

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
        // Values mirror the theme.css tokens. Monaco cannot read CSS custom
        // properties, so these are duplicated deliberately: the accent is the
        // only chromatic token, and comments recede to --text-faint so they
        // never compete with code.
        rules: [
            { token: 'comment', foreground: '63636d', fontStyle: 'italic' },
            { token: 'keyword', foreground: '6d9ee8' },
            { token: 'keyword.directive', foreground: '8e8e99' },
            { token: 'number', foreground: 'd0a24f' },
            { token: 'string', foreground: '8fbf7f' },
            { token: 'type', foreground: '7fc8c8' },
            { token: 'type.identifier', foreground: '7fc8c8' },
            { token: 'identifier', foreground: 'c8c8d0' },
        ],
        colors: {
            'editor.background': '#1a1a1e',
            'editor.foreground': '#c8c8d0',
            'editorLineNumber.foreground': '#4a4a56',
            'editorLineNumber.activeForeground': '#8e8e99',
            'editorGutter.background': '#1a1a1e',
            'editor.lineHighlightBackground': '#212126',
            'editor.selectionBackground': '#2c4b72',
            'editorIndentGuide.background1': '#2a2a32',
            'editorWidget.background': '#212126',
            'editorSuggestWidget.background': '#212126',
            'editorSuggestWidget.border': '#2b2b33',
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
