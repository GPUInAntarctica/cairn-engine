// Cairn Engine editor - entry point.
// Shows the project browser first; once a project is open, mounts the editor.
'use strict';

import { getPrefs, setLanguagePref } from './prefs.js';
import { initI18n, setLanguage, t, language } from './i18n.js';
import { openProject, engineVersion } from './state.js';
import { ProjectBrowser } from './ui/projectBrowser.js';
import { mountEditor } from './ui/app.js';

async function main() {
    const prefs = await getPrefs();
    const lang = prefs.language || (navigator.language.startsWith('zh') ? 'zh-CN' : 'en');
    initI18n(lang);
    if (lang !== (prefs.language || '')) setLanguagePref(lang);

    const root = document.getElementById('root');

    // Automated test hook: open a project directly without the browser UI.
    if (process.env.CAIRN_TEST_PROJECT) {
        const { ipc } = await import('./ipc.js');
        const r = await ipc.readProject(process.env.CAIRN_TEST_PROJECT);
        if (r.ok) {
            openProject(r.project);
            root.innerHTML = '';
            mountEditor(root, prefs);
            return;
        }
    }

    const browser = new ProjectBrowser(root, {
        engineVersion: engineVersion(),
        onOpen: async projectDoc => {
            openProject(projectDoc);
            root.innerHTML = '';
            mountEditor(root, prefs);
        },
    });
    browser.render();
}

main();
