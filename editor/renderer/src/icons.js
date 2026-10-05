// Cairn Engine editor - inline SVG icon set (stroke-based, 14px grid).
'use strict';

// One drawing grid (16x16 viewBox) and one stroke weight for every glyph.
// `icon(name, size)` scales the rendered box only: keeping the weight constant
// across sizes is what stops a 20px tile icon from looking heavier than a
// 14px row icon. Draw sizes from the grid - 14 (row), 16 (tool), 20 (tile).
const STROKE = 'fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"';

const paths = {
    logo: (size = 15) =>
        `<svg width="${size}" height="${size}" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
            <path d="M8 2 L11.2 6.6 L10 6.6 M8 2 L4.8 6.6 L6 6.6" />
            <path d="M3.4 7.4 L8 13.6 L12.6 7.4 L3.4 7.4 Z" />
            <path d="M5.9 7.4 L8 10.4 L10.1 7.4" opacity="0.55"/>
        </svg>`,
    select: `<path d="M4 2.5 L12.5 8 L8.6 8.9 L10.4 13 L8.7 13.7 L6.9 9.6 L4 12.3 Z"/>`,
    move: `<path d="M8 1.5 V14.5 M1.5 8 H14.5 M8 1.5 L6 3.5 M8 1.5 L10 3.5 M8 14.5 L6 12.5 M8 14.5 L10 12.5 M1.5 8 L3.5 6 M1.5 8 L3.5 10 M14.5 8 L12.5 6 M14.5 8 L12.5 10"/>`,
    rotate: `<path d="M13.5 8 A5.5 5.5 0 1 1 8 2.5 M8 2.5 L10.5 1.2 M8 2.5 L10 4.8"/>`,
    scale: `<path d="M2 14 V6 H10 M14 2 L8.5 7.5 M14 2 H10.5 M14 2 V5.5 M5 11 L11 5"/>`,
    play: `<path d="M4.5 2.8 L12.8 8 L4.5 13.2 Z" fill="currentColor" stroke="none"/>`,
    pause: `<path d="M5 3 V13 M11 3 V13" stroke-width="2.2"/>`,
    stop: `<rect x="4" y="4" width="8" height="8" fill="currentColor" stroke="none"/>`,
    step: `<path d="M3.5 3 L9.5 8 L3.5 13 Z" fill="currentColor" stroke="none"/><path d="M12.5 3 V13" stroke-width="1.8"/>`,
    cube: `<path d="M8 1.8 L14 5 V11 L8 14.2 L2 11 V5 Z M2 5 L8 8.2 L14 5 M8 8.2 V14.2"/>`,
    sprite: `<rect x="2.5" y="2.5" width="11" height="11" rx="1"/><path d="M2.5 11 L6.5 7 L9 9.5 L11.5 7 L13.5 9"/>`,
    camera: `<rect x="1.8" y="4.5" width="9" height="7" rx="1.2"/><path d="M10.8 7.5 L14.2 5.5 V10.5 L10.8 8.5"/>`,
    light: `<circle cx="8" cy="8" r="3"/><path d="M8 1.5 V3 M8 13 V14.5 M1.5 8 H3 M13 8 H14.5 M3.4 3.4 L4.5 4.5 M11.5 11.5 L12.6 12.6 M12.6 3.4 L11.5 4.5 M4.5 11.5 L3.4 12.6"/>`,
    text: `<path d="M3 3.5 H13 M8 3.5 V13 M6 13 H10"/>`,
    script: `<path d="M5.5 4 L2.5 8 L5.5 12 M10.5 4 L13.5 8 L10.5 12"/>`,
    physics: `<circle cx="8" cy="8" r="2" fill="currentColor" stroke="none"/><path d="M8 2 A6 6 0 0 1 14 8 M14 8 A6 6 0 0 1 8 14 M8 14 A6 6 0 0 1 2 8 M2 8 A6 6 0 0 1 8 2" opacity="0.5"/>`,
    folder: `<path d="M1.8 4 A1 1 0 0 1 2.8 3 H6 L7.5 4.8 H13.2 A1 1 0 0 1 14.2 5.8 V12 A1 1 0 0 1 13.2 13 H2.8 A1 1 0 0 1 1.8 12 Z"/>`,
    image: `<rect x="2" y="2.5" width="12" height="11" rx="1"/><circle cx="5.6" cy="6" r="1.2"/><path d="M2.5 11.5 L6.5 8 L9 10.5 L11 8.8 L13.5 11"/>`,
    scene: `<path d="M2.5 13 L6 3 L9.5 10.5 L11 7.5 L13.5 13 Z"/>`,
    mesh: `<path d="M8 2 L13.5 5.5 V10.5 L8 14 L2.5 10.5 V5.5 Z M2.5 5.5 L8 9 L13.5 5.5 M8 9 V14 M2.5 10.5 L8 7.5 M13.5 10.5 L8 7.5" opacity="0.9"/>`,
    close: `<path d="M4 4 L12 12 M12 4 L4 12"/>`,
    add: `<path d="M8 3 V13 M3 8 H13"/>`,
    remove: `<path d="M3.5 8 H12.5"/>`,
    eye: `<path d="M1.5 8 C3.5 4.5 5.5 3.5 8 3.5 C10.5 3.5 12.5 4.5 14.5 8 C12.5 11.5 10.5 12.5 8 12.5 C5.5 12.5 3.5 11.5 1.5 8 Z"/><circle cx="8" cy="8" r="2"/>`,
    brush: `<path d="M13.5 2.5 C10 4 7 6.5 5.5 9 L7 10.5 C9.5 9 12 6 13.5 2.5 Z M5 9.5 C3.5 10 3 11 2.5 13.5 C5 13 6 12.5 6.5 11"/>`,
    eraser: `<path d="M9.5 2.5 L13.5 6.5 L7 13 H4 L2.5 11.5 Z M5.5 6.5 L10 11"/>`,
    fill: `<path d="M8 2 L13 7 L8.5 11.5 A1.4 1.4 0 0 1 6.5 11.5 L3 8 Z M3 8 H11"/><path d="M13.5 10.5 C14.5 12 14.5 13 13.8 13.4 C13.2 13.8 12.5 13.3 12.7 12.4 C12.8 11.8 13.2 11.2 13.5 10.5 Z" fill="currentColor"/>`,
    line: `<path d="M3 13 L13 3 M3 13 L4.5 13 M3 13 L3 11.5 M13 3 L11.5 3 M13 3 L13 4.5"/>`,
    rect: `<rect x="3" y="3.5" width="10" height="9"/>`,
    ellipse: `<ellipse cx="8" cy="8" rx="5.5" ry="4.5"/>`,
    picker: `<path d="M9 5.5 L10.5 7 L5.5 12 L3.5 12.5 L4 10.5 Z M9 5.5 L12 2.5 L13.5 4 L10.5 7"/>`,
    hand: `<path d="M5 8 V3.8 A1 1 0 0 1 7 3.8 V7.5 M7 7.5 V3 A1 1 0 0 1 9 3 V7.5 M9 7.5 V3.8 A1 1 0 0 1 11 3.8 V9 C11 11.5 9.5 13.5 7.5 13.5 C5.5 13.5 4 12 3.5 9.5 L3 7.5 A0.9 0.9 0 0 1 4.7 7"/>`,
    gear: `<circle cx="8" cy="8" r="2.2"/><path d="M8 1.8 L8.8 3.4 L10.6 3 L11.4 4.6 L13 5.4 L12.6 7.2 L14 8 L12.6 8.8 L13 10.6 L11.4 11.4 L10.6 13 L8.8 12.6 L8 14.2 L7.2 12.6 L5.4 13 L4.6 11.4 L3 10.6 L3.4 8.8 L2 8 L3.4 7.2 L3 5.4 L4.6 4.6 L5.4 3 L7.2 3.4 Z"/>`,
    console: `<rect x="1.8" y="2.8" width="12.4" height="10.4" rx="1"/><path d="M4.5 6 L6.5 8 L4.5 10 M8 10.5 H11.5"/>`,
    normalMap: `<path d="M3 12 L13 4 M13 4 H9.8 M13 4 V7.2"/><circle cx="5" cy="6" r="2" opacity="0.7"/><circle cx="11" cy="10" r="2" opacity="0.7"/>`,
    export: `<path d="M8 2 V10 M8 2 L5.5 4.5 M8 2 L10.5 4.5 M3 8 V13 H13 V8"/>`,
    playWindow: `<rect x="2" y="2.8" width="12" height="10.4" rx="1"/><path d="M6.5 5.8 L10.5 8 L6.5 10.2 Z" fill="currentColor" stroke="none"/>`,
    chevronRight: `<path d="M6 3.5 L10.5 8 L6 12.5"/>`,
    chevronDown: `<path d="M3.5 6 L8 10.5 L12.5 6"/>`,
};

export function icon(name, size = 14) {
    if (name === 'logo') return logo(size);
    const body = paths[name] ?? paths.cube;
    return `<svg width="${size}" height="${size}" viewBox="0 0 16 16" ${STROKE}>${body}</svg>`;
}

function logo(size = 15) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
            <path d="M8 2 L11.2 6.6 L10 6.6 M8 2 L4.8 6.6 L6 6.6" />
            <path d="M3.4 7.4 L8 13.6 L12.6 7.4 L3.4 7.4 Z" />
            <path d="M5.9 7.4 L8 10.4 L10.1 7.4" opacity="0.55"/>
        </svg>`;
}
