// Cairn Engine editor - project management (main process).
//
// A project is a directory containing project.cproj:
//   { "format": "cairn-project", "version": 1, "name": "...", "template": "..." }
// with Assets/ for user content and Library/ for build artifacts.
'use strict';

const fs = require('fs');
const path = require('path');
const preferences = require('./preferences');

const TEMPLATES_DIR = path.join(__dirname, '..', '..', 'templates');

function projectFile(dir) { return path.join(dir, 'project.cproj'); }

function readProject(projectDir) {
    const file = projectFile(projectDir);
    try {
        const doc = JSON.parse(fs.readFileSync(file, 'utf8'));
        if (doc.format !== 'cairn-project') return { ok: false, error: 'not a Cairn project' };
        doc.path = projectDir;
        return { ok: true, project: doc };
    } catch (err) {
        return { ok: false, error: `cannot read project: ${err.message}` };
    }
}

function listTemplates() {
    try {
        return fs.readdirSync(TEMPLATES_DIR)
            .filter(name => fs.existsSync(path.join(TEMPLATES_DIR, name, 'template.json')))
            .map(name => {
                try { return JSON.parse(fs.readFileSync(path.join(TEMPLATES_DIR, name, 'template.json'), 'utf8')); }
                catch { return null; }
            })
            .filter(Boolean);
    } catch {
        return [];
    }
}

function copyDir(src, dst) {
    fs.mkdirSync(dst, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
        const s = path.join(src, entry.name);
        const d = path.join(dst, entry.name);
        if (entry.isDirectory()) copyDir(s, d);
        else fs.copyFileSync(s, d);
    }
}

// Generates a tiny PNG without dependencies (solid color or 2x2 checker),
// used to seed template texture assets.
function writePng(filePath, width, height, pixelFn) {
    const zlib = require('zlib');
    const bytesPerRow = width * 4 + 1;
    const raw = Buffer.alloc(bytesPerRow * height);
    for (let y = 0; y < height; ++y) {
        raw[y * bytesPerRow] = 0;  // filter: none
        for (let x = 0; x < width; ++x) {
            const [r, g, b, a] = pixelFn(x, y);
            const o = y * bytesPerRow + 1 + x * 4;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b; raw[o + 3] = a;
        }
    }
    const idat = zlib.deflateSync(raw);
    const chunks = [];
    const chunk = (type, data) => {
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const crcTable = [];
        for (let n = 0; n < 256; ++n) {
            let c = n;
            for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
            crcTable[n] = c >>> 0;
        }
        let crc = 0xffffffff;
        for (const byte of body) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
        crc = (crc ^ 0xffffffff) >>> 0;
        const crcBuf = Buffer.alloc(4);
        crcBuf.writeUInt32BE(crc);
        chunks.push(len, body, crcBuf);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunks.push(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
    chunk('IHDR', ihdr);
    chunk('IDAT', idat);
    chunk('IEND', Buffer.alloc(0));
    fs.writeFileSync(filePath, Buffer.concat(chunks));
}

function createProject(options, prefs) {
    const { parentDir, name, templateId } = options;
    const projectDir = path.join(parentDir, name);
    if (fs.existsSync(projectDir)) return { ok: false, error: 'directory already exists' };
    if (!/^[\w][\w\- ]*$/.test(name)) return { ok: false, error: 'invalid project name' };

    try {
        fs.mkdirSync(projectDir, { recursive: true });
        for (const sub of ['Assets/Scenes', 'Assets/Scripts', 'Assets/Textures', 'Library']) {
            fs.mkdirSync(path.join(projectDir, sub), { recursive: true });
        }
        fs.writeFileSync(projectFile(projectDir), JSON.stringify({
            format: 'cairn-project',
            version: 1,
            name,
            template: templateId,
            engineVersion: '0.1.31',
            createdAt: new Date().toISOString(),
        }, null, 2));

        // Template content.
        const templateDir = path.join(TEMPLATES_DIR, templateId);
        if (fs.existsSync(templateDir)) copyDir(templateDir, projectDir);

        // Seed texture assets (procedural, so the repo carries no binaries).
        const textures = path.join(projectDir, 'Assets', 'Textures');
        writePng(path.join(textures, 'checker.png'), 256, 256, (x, y) => {
            const cell = ((x >> 5) + (y >> 5)) & 1;
            return cell ? [232, 232, 236, 255] : [70, 72, 80, 255];
        });
        if (templateId === 'project-2d') {
            writePng(path.join(textures, 'player.png'), 64, 64, (x, y) => {
                const dx = x - 32, dy = y - 32;
                return dx * dx + dy * dy < 900 ? [240, 196, 89, 255] : [0, 0, 0, 0];
            });
            writePng(path.join(textures, 'ground.png'), 64, 64, () => [96, 122, 90, 255]);
            writePng(path.join(textures, 'crate.png'), 64, 64, (x, y) => {
                const border = x < 4 || y < 4 || x > 59 || y > 59;
                return border ? [66, 48, 32, 255] : [160, 118, 74, 255];
            });
        }

        preferences.touchRecent(prefs, projectDir, name);
        return { ok: true, project: readProject(projectDir).project };
    } catch (err) {
        return { ok: false, error: err.message };
    }
}

module.exports = { readProject, listTemplates, createProject, projectFile };
