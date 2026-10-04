// Cairn Engine editor - localization.
// Dictionaries live in i18n/en.js and i18n/zh-CN.js. `t(key)` falls back to
// English, then to the key itself. Components re-render on language change.
'use strict';

import { dict as en } from './i18n/en.js';
import { dict as zhCN } from './i18n/zh-CN.js';

const dicts = { en, 'zh-CN': zhCN };

let current = 'en';
const listeners = new Set();

export function initI18n(lang) {
    current = dicts[lang] ? lang : 'en';
}

export function setLanguage(lang) {
    if (!dicts[lang] || lang === current) return;
    current = lang;
    for (const fn of listeners) fn(current);
}

export function language() { return current; }
export function languages() { return Object.keys(dicts); }
export function onLanguageChange(fn) { listeners.add(fn); return () => listeners.delete(fn); }

export function t(key, vars) {
    let text = dicts[current][key] ?? en[key] ?? key;
    if (vars) {
        for (const [k, v] of Object.entries(vars)) text = text.replace(`{${k}}`, v);
    }
    return text;
}
