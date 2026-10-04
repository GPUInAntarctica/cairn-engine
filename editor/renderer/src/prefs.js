// Cairn Engine editor - language preference helper (avoids cycles).
'use strict';

import { ipc } from './ipc.js';

export async function getPrefs() {
    return ipc.getPrefs();
}

export async function setLanguagePref(lang) {
    return ipc.setPrefs({ language: lang });
}
