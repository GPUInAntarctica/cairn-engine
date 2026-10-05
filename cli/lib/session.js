// Cairn CLI - session state shared by all commands.
//
// Owns project discovery, lazy loading of the N-API addon, the engine viewport
// and path translation between CLI paths and the engine's project-relative
// asset references. A single Session (and therefore a single engine instance)
// is reused for the whole lifetime of the process, including `cairn exec`.
'use strict';

const fs = require('fs');
const path = require('path');

const { notFound, failed, usage } = require('./errors');
const projectTools = require(path.join(__dirname, '..', '..', 'editor', 'main', 'project.js'));
const buildTools = require(path.join(__dirname, '..', '..', 'editor', 'main', 'build.js'));

const ROOT = path.join(__dirname, '..', '..');
const ADDON_PATH = path.join(ROOT, 'build', 'cairn_engine_addon.node');

class Session {
    constructor(opts = {}) {
        this.opts = opts;
        this.cwd = opts.cwd || process.cwd();
        this.width = opts.width || 960;
        this.height = opts.height || 540;
        this._project = undefined;
        this._engine = undefined;
        this._scenePath = null;
        this._logs = [];
        this._logsDrained = false;
    }

    // ------------------------------------------------------------- projects
    tryProject() {
        if (this._project !== undefined) return this._project;
        const candidates = [];
        if (this.opts.project) candidates.push(path.resolve(this.cwd, this.opts.project));
        if (process.env.CAIRN_PROJECT) candidates.push(path.resolve(this.cwd, process.env.CAIRN_PROJECT));
        candidates.push(this.cwd);
        for (const dir of candidates) {
            if (fs.existsSync(path.join(dir, 'project.cproj'))) { this._project = dir; return dir; }
        }
        for (const dir of candidates) {
            if (fs.existsSync(dir) && fs.statSync(dir).isDirectory()) { this._project = dir; return dir; }
        }
        this._project = null;
        return null;
    }

    requireProject() {
        const dir = this.tryProject();
        if (!dir) {
            throw usage('no project directory: pass --project <dir> (or run inside a project)');
        }
        const read = projectTools.readProject(dir);
        if (!read.ok) throw notFound(read.error, { project: dir });
        return dir;
    }

    projectInfo() {
        const dir = this.requireProject();
        return projectTools.readProject(dir).project;
    }

    // ---------------------------------------------------------------- engine
    addon() {
        if (!fs.existsSync(ADDON_PATH)) {
            throw failed('engine_unavailable',
                `native addon not found at ${ADDON_PATH} - run \`npm run build\` first`);
        }
        // eslint-disable-next-line global-require
        return require(ADDON_PATH);
    }

    engine() {
        if (this._engine) return this._engine;
        const addon = this.addon();
        const engine = new addon.Engine();
        engine.setProjectRoot(this.tryProject() || this.cwd);
        engine.setViewportSize(this.width, this.height);
        // Keep stdout a clean JSON channel; logs are read with runtime.logs.
        if (engine.setLogEcho) engine.setLogEcho(this.opts.logEcho === true);
        this._engine = engine;
        if (this.opts.scene) this.loadScene(this.opts.scene);
        return engine;
    }

    engineVersion() { return this.addon().engineVersion; }

    // ----------------------------------------------------------------- paths
    resolveScenePath(target) {
        const str = String(target);
        if (path.isAbsolute(str)) return str;
        const fromCwd = path.resolve(this.cwd, str);
        if (fs.existsSync(fromCwd)) return fromCwd;
        const project = this.tryProject();
        if (project) {
            const fromProject = path.resolve(project, str);
            if (fs.existsSync(fromProject)) return fromProject;
            return fromProject;
        }
        return fromCwd;
    }

    // Engine asset references are project-root relative POSIX paths.
    toProjectRelative(absolute) {
        const project = this.tryProject();
        if (!project) return absolute.split(path.sep).join('/');
        const rel = path.relative(project, absolute);
        if (rel.startsWith('..')) return absolute.split(path.sep).join('/');
        return rel.split(path.sep).join('/');
    }

    // Accepts a project-relative asset path, a path relative to cwd, or an
    // absolute path and returns the absolute form.
    resolveAssetPath(target) {
        const str = String(target);
        if (path.isAbsolute(str)) return str;
        const project = this.tryProject();
        if (project) {
            const fromProject = path.resolve(project, str);
            if (fs.existsSync(fromProject)) return fromProject;
            const fromCwd = path.resolve(this.cwd, str);
            if (fs.existsSync(fromCwd)) return fromCwd;
            return fromProject;
        }
        return path.resolve(this.cwd, str);
    }

    // Output paths may be given relative to the project (Assets/...) or cwd.
    resolveOutputPath(target) {
        const str = String(target);
        if (path.isAbsolute(str)) return str;
        const project = this.tryProject();
        if (project && (str.startsWith('Assets/') || str.startsWith('Assets\\'))) {
            return path.resolve(project, str);
        }
        return path.resolve(this.cwd, str);
    }

    requireSceneLoaded() {
        const engine = this.engine();
        if (!this._scenePath) {
            throw usage('no scene loaded: pass --scene <file.cscene> or run `cairn scene.new` first');
        }
        return { engine, scenePath: this._scenePath };
    }

    loadScene(target) {
        const engine = this.engine();
        const file = this.resolveScenePath(target);
        const result = engine.loadScene(file);
        if (!result.ok) {
            throw failed('scene_load_failed', result.error, { scene: file });
        }
        this._scenePath = file;
        this._logs = [];
        return { path: file, name: engine.getSceneTree().name, entities: engine.getSceneTree().entities.length };
    }

    saveScene(target) {
        const engine = this.engine();
        if (!this._scenePath && !target) {
            throw usage('no scene loaded: pass --scene <file.cscene> or run `cairn scene.new` first');
        }
        const file = target ? this.resolveScenePath(target) : this._scenePath;
        const result = engine.saveScene(file);
        if (!result.ok) throw failed('scene_save_failed', result.error, { scene: file });
        this._scenePath = file;
        return { path: file };
    }

    loadImage(target) {
        // eslint-disable-next-line global-require
        const imaging = require('./imaging.js');
        const file = this.resolveAssetPath(target);
        const doc = imaging.loadImage(file);
        this._image = doc;
        this._imagePath = file;
        return { path: file, width: doc.width, height: doc.height, layers: doc.layers.length };
    }

    // ---------------------------------------------------------------- scripts
    async buildScripts(projectDir, compiler) {
        const cxx = compiler || await detectCompiler();
        if (!cxx) throw failed('compiler_missing', 'no C++ compiler found (tried g++, clang++)');
        return Object.assign({ compiler: cxx }, await buildTools.buildScripts(projectDir, cxx));
    }
}

function detectCompiler() {
    const { spawnSync } = require('child_process');
    const prefs = require(path.join(ROOT, 'editor', 'main', 'preferences.js'));
    const explicit = (() => { try { return prefs.load().compiler; } catch { return ''; } })();
    const candidates = process.platform === 'win32' ? [explicit, 'cl', 'clang++', 'g++'] : [explicit, 'g++', 'clang++'];
    for (const c of candidates) {
        if (!c) continue;
        const r = spawnSync(c, ['--version'], { encoding: 'utf8', shell: process.platform === 'win32' });
        if (r.status === 0) return c;
    }
    return null;
}

module.exports = { Session, ROOT, ADDON_PATH, detectCompiler };
