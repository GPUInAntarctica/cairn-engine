// Cairn CLI - project, template, asset and export commands.
'use strict';

const fs = require('fs');
const path = require('path');

const { def } = require('../lib/registry');
const { notFound, invalidArg } = require('../lib/errors');
const projectTools = require('../../editor/main/project.js');
const exportTools = require('../../editor/main/export.js');
const preferences = require('../../editor/main/preferences.js');

const TEXTURE_EXTS = ['.png', '.jpg', '.jpeg', '.bmp'];
const MODEL_EXTS = ['.obj'];

def('project.info', {
    summary: 'Read project.cproj of the current project.',
    args: {},
    returns: '{format, version, name, template, engineVersion, path, scenes:[], scripts:[], textures:[], sizeBytes}',
    needsProject: true,
    examples: ['cairn --project ./game project.info'],
    run(ctx) {
        const info = ctx.session.projectInfo();
        return Object.assign({}, info, {
            scenes: listFiles(path.join(info.path, 'Assets', 'Scenes'), ['.cscene']),
            scripts: listFiles(path.join(info.path, 'Assets', 'Scripts'), ['.cpp', '.cc', '.cxx']),
            textures: listFiles(path.join(info.path, 'Assets', 'Textures'), TEXTURE_EXTS),
            library: fs.existsSync(path.join(info.path, 'Library'))
                ? fs.readdirSync(path.join(info.path, 'Library'))
                : [],
        });
    },
});

def('project.list', {
    summary: 'List recent projects from the preferences store.',
    args: {},
    returns: '[{path, name, lastOpened}]',
    examples: ['cairn project.list'],
    run() {
        return preferences.load().recentProjects || [];
    },
});

def('project.templates', {
    summary: 'List available project templates.',
    args: {},
    returns: '[{id, titleKey, descKey}]',
    examples: ['cairn project.templates'],
    run() {
        return projectTools.listTemplates();
    },
});

def('project.create', {
    summary: 'Create a new project directory from a template (seeds Assets and starter textures).',
    args: {
        name: { type: 'string', required: true, desc: 'Project name; also the directory name.' },
        parent: { type: 'path', desc: 'Parent directory (default: current directory).' },
        template: { type: 'string', default: 'project-3d', desc: 'Template id from project.templates.' },
    },
    returns: '{ok, project:{path, name, template}}',
    examples: ['cairn project.create --name MyGame --parent ~/games --template project-2d'],
    async run(ctx, args) {
        const parent = args.parent || ctx.cwd;
        const result = projectTools.createProject(
            { parentDir: parent, name: args.name, templateId: args.template },
            preferences.load());
        if (!result.ok) throw invalidArg(result.error, { name: args.name, parent, template: args.template });
        return result;
    },
});

def('project.scenes', {
    summary: 'List scene files in Assets/Scenes.',
    args: {},
    returns: '[string]',
    needsProject: true,
    examples: ['cairn --project ./game project.scenes'],
    run(ctx) {
        const dir = path.join(ctx.session.requireProject(), 'Assets', 'Scenes');
        return listFiles(dir, ['.cscene']);
    },
});

def('project.scripts', {
    summary: 'List gameplay script sources in Assets/Scripts.',
    args: {},
    returns: '[{path, bytes}]',
    needsProject: true,
    examples: ['cairn --project ./game project.scripts'],
    run(ctx) {
        const dir = path.join(ctx.session.requireProject(), 'Assets', 'Scripts');
        return listFiles(dir, ['.cpp', '.cc', '.cxx']).map(rel => ({
            path: `Assets/Scripts/${rel}`,
            bytes: fs.statSync(path.join(dir, rel)).size,
        }));
    },
});

def('assets.list', {
    summary: 'List project assets (textures or models) as project-relative paths.',
    args: {
        kind: { type: 'enum', enum: ['texture', 'model', 'all'], default: 'texture', desc: 'Asset kind to list.' },
    },
    returns: '[string]',
    needsProject: true,
    examples: ['cairn --project ./game assets.list --kind texture'],
    run(ctx, args) {
        const root = path.join(ctx.session.requireProject(), 'Assets');
        const exts = args.kind === 'model' ? MODEL_EXTS : args.kind === 'all' ? TEXTURE_EXTS.concat(MODEL_EXTS) : TEXTURE_EXTS;
        return listFiles(root, exts).map(rel => `Assets/${rel}`);
    },
});

def('assets.info', {
    summary: 'Check that an asset reference resolves, and report its size.',
    args: {
        asset: { type: 'string', required: true, desc: 'Project-relative asset path, e.g. Assets/Textures/checker.png.' },
    },
    returns: '{exists, path, bytes, relative}',
    examples: ['cairn assets.info Assets/Textures/checker.png'],
    run(ctx, args) {
        const abs = ctx.session.resolveAssetPath(args.asset);
        const exists = fs.existsSync(abs) && fs.statSync(abs).isFile();
        return {
            exists,
            path: abs,
            relative: exists ? ctx.session.toProjectRelative(abs) : args.asset,
            bytes: exists ? fs.statSync(abs).size : 0,
        };
    },
});

def('assets.assetsReload', {
    summary: 'Drop cached textures and meshes so the next access re-reads from disk.',
    args: {},
    returns: '{reloaded:true}',
    needsEngine: true,
    examples: ['cairn assets.assetsReload'],
    run(ctx) {
        ctx.session.engine().reloadAssets();
        return { reloaded: true };
    },
});

def('assets.database', {
    summary: 'List the asset database: stable GUID, project-relative path and importer.',
    args: {},
    returns: '[{guid, path, importer}]',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.database'],
    run(ctx) {
        return ctx.session.engine().assetsList();
    },
});

def('assets.guid', {
    summary: 'Return the stable GUID of an asset (creates its .meta on first use).',
    args: {
        asset: { type: 'string', required: true, desc: 'Project-relative asset path, e.g. Assets/Textures/checker.png.' },
    },
    returns: '{guid, path}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.guid --asset Assets/Textures/checker.png'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const rel = ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.asset));
        let guid = engine.assetGuid(rel);
        if (!guid) {
            engine.assetImport(ctx.session.resolveAssetPath(args.asset), rel);
            guid = engine.assetGuid(rel);
        }
        if (!guid) throw notFound(`cannot resolve asset '${args.asset}'`, { asset: args.asset });
        return { guid, path: rel };
    },
});

def('assets.resolve', {
    summary: 'Resolve a GUID back to its current project-relative path (survives renames).',
    args: {
        guid: { type: 'string', required: true, desc: '32-hex asset GUID from a .meta file.' },
    },
    returns: '{guid, path}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.resolve --guid 3f2a...'],
    run(ctx, args) {
        const path = ctx.session.engine().assetPath(args.guid);
        if (!path) throw notFound(`no asset with guid '${args.guid}'`, { guid: args.guid });
        return { guid: args.guid, path };
    },
});

def('assets.meta.get', {
    summary: 'Read the .meta sidecar (GUID, importer, settings) of an asset.',
    args: {
        asset: { type: 'string', required: true, desc: 'Project-relative asset path.' },
    },
    returns: '{guid, importer, version, settings}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.meta.get --asset Assets/Textures/checker.png'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const rel = ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.asset));
        engine.assetGuid(rel);  // ensure the meta exists
        const meta = engine.assetMeta(rel);
        if (!meta) throw notFound(`asset '${args.asset}' has no meta`, { asset: args.asset });
        return meta;
    },
});

def('assets.meta.set', {
    summary: 'Write importer settings into an asset .meta (e.g. {"colorSpace":"linear"}).',
    args: {
        asset: { type: 'string', required: true, desc: 'Project-relative asset path.' },
        settings: { type: 'json', required: true, desc: 'Settings object merged into the meta.' },
    },
    returns: '{ok, guid}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.meta.set --asset Assets/Textures/n.png --settings \'{"colorSpace":"linear"}\''],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const rel = ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.asset));
        engine.assetGuid(rel);
        const result = engine.assetMetaSet(rel, args.settings);
        if (!result || !result.ok) throw invalidArg(result && result.error || 'meta write failed', { asset: args.asset });
        return { ok: true, guid: result.guid };
    },
});

def('assets.import', {
    summary: 'Copy an external file into the project Assets tree and create its .meta.',
    args: {
        source: { type: 'path', required: true, desc: 'File to import from anywhere on disk.' },
        dest: { type: 'string', desc: 'Project-relative destination, e.g. Assets/Textures/rock.png.' },
    },
    returns: '{ok, guid, path}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.import --source ~/rock.png --dest Assets/Textures/rock.png'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const source = ctx.session.resolveAssetPath(args.source);
        if (!fs.existsSync(source)) throw notFound(`source not found: ${args.source}`, { source });
        const dest = args.dest || defaultImportDest(args.source);
        const rel = ctx.session.toProjectRelative(path.resolve(ctx.session.tryProject(), dest));
        const result = engine.assetImport(source, rel);
        if (!result.ok) throw invalidArg(result.error, { source, dest });
        return { ok: true, guid: result.guid, path: rel };
    },
});

def('assets.move', {
    summary: 'Rename/move an asset inside the project; its GUID (and scene references) survive.',
    args: {
        from: { type: 'string', required: true, desc: 'Current project-relative path.' },
        to: { type: 'string', required: true, desc: 'New project-relative path.' },
    },
    returns: '{ok, guid, path}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.move --from Assets/Textures/a.png --to Assets/Textures/b.png'],
    run(ctx, args) {
        const from = ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.from));
        const to = ctx.session.toProjectRelative(path.resolve(ctx.session.tryProject(), args.to));
        const result = ctx.session.engine().assetMove(from, to);
        if (!result.ok) throw notFound(result.error, { from, to });
        return { ok: true, guid: result.guid, path: result.path };
    },
});

def('assets.delete', {
    summary: 'Delete an asset and its .meta sidecar.',
    args: {
        asset: { type: 'string', required: true, desc: 'Project-relative asset path.' },
    },
    returns: '{ok}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game assets.delete --asset Assets/Textures/old.png'],
    run(ctx, args) {
        const rel = ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.asset));
        const result = ctx.session.engine().assetDelete(rel);
        if (!result.ok) throw notFound(result.error, { asset: args.asset });
        return { ok: true };
    },
});

const IMPORT_DIRS = {
    texture: 'Textures', model: 'Models', audio: 'Audio', script: 'Scripts', scene: 'Scenes',
};

function defaultImportDest(source) {
    const ext = path.extname(source).toLowerCase();
    const sub = ['Assets', IMPORT_DIRS[importerOf(ext)] || 'Raw'].filter(Boolean).join('/');
    return `${sub}/${path.basename(source)}`;
}

function importerOf(ext) {
    if (['.png', '.jpg', '.jpeg', '.bmp', '.tga'].includes(ext)) return 'texture';
    if (['.obj', '.gltf', '.glb', '.fbx'].includes(ext)) return 'model';
    if (['.wav', '.ogg', '.mp3'].includes(ext)) return 'audio';
    return 'other';
}

def('project.exportPlayer', {
    summary: 'Build gameplay scripts and package a standalone Electron player directory.',
    args: {
        target: { type: 'path', required: true, desc: 'Output directory for the player build.' },
        compiler: { type: 'string', desc: 'C++ compiler to use (default: auto-detect).' },
        skipScripts: { type: 'bool', default: false, desc: 'Package without compiling scripts first.' },
    },
    returns: '{ok, dir, scripts:{ok, output}}',
    needsProject: true,
    examples: ['cairn --project ./game project.exportPlayer --target ./build/player'],
    async run(ctx, args) {
        const projectDir = ctx.session.requireProject();
        let scripts = { ok: true, output: path.join(projectDir, 'Library', 'GameScripts' + (process.platform === 'win32' ? '.dll' : '.so')) };
        if (!args.skipScripts) {
            scripts = await ctx.session.buildScripts(projectDir, args.compiler);
            if (!scripts.ok) {
                const message = scripts.noSources ? 'no gameplay scripts found in Assets/Scripts' : 'script build failed';
                throw notFound(message, { diagnostics: scripts.diagnostics });
            }
        }
        const result = await exportTools.exportPlayer(projectDir, args.target,
            async (dir, compiler) => (args.skipScripts ? { ok: true, output: '' } : ctx.session.buildScripts(dir, compiler)),
            args.compiler);
        if (!result.ok) throw notFound(result.error, { project: projectDir, target: args.target });
        return { ok: true, dir: result.dir, scripts: { ok: scripts.ok, output: scripts.output } };
    },
});

def('project.preferences', {
    summary: 'Read or write editor preferences (language, compiler, recent projects).',
    args: {
        set: { type: 'json', desc: 'Object of preference keys to write, e.g. {"compiler":"clang++"}.' },
    },
    returns: '{language, compiler, autosave, showStats, lastProject, recentProjects}',
    examples: ['cairn project.preferences', 'cairn project.preferences --set \'{"compiler":"g++"}\''],
    run(ctx, args) {
        const prefs = preferences.load();
        if (args.set) {
            for (const [key, value] of Object.entries(args.set)) {
                if (!(key in prefs)) throw invalidArg(`unknown preference '${key}'`, { known: Object.keys(prefs) });
                prefs[key] = value;
            }
            preferences.save(prefs);
        }
        return prefs;
    },
});

function listFiles(root, exts) {
    const out = [];
    const walk = (dir, rel) => {
        let entries;
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
        for (const e of entries) {
            const full = path.join(dir, e.name);
            const relPath = rel ? rel + '/' + e.name : e.name;
            if (e.isDirectory()) {
                if (e.name !== 'Library') walk(full, relPath);
            } else if (exts.some(ext => e.name.toLowerCase().endsWith(ext))) {
                out.push(relPath);
            }
        }
    };
    walk(root, '');
    return out.sort();
}

module.exports = { listFiles };
