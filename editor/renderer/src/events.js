// Cairn Engine editor - minimal event bus.
'use strict';

const listeners = new Map();

export function on(event, fn) {
    if (!listeners.has(event)) listeners.set(event, new Set());
    listeners.get(event).add(fn);
    return () => listeners.get(event).delete(fn);
}

export function emit(event, payload) {
    const set = listeners.get(event);
    if (set) for (const fn of set) fn(payload);
}
