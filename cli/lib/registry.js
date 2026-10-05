// Cairn CLI - command registry, argument schema and coercion.
//
// Every command is declared once with a machine-readable arg schema so that
// `cairn schema`, `cairn help <cmd>` and batch mode all describe the same
// contract the dispatcher enforces.
'use strict';

const fs = require('fs');
const path = require('path');
const { usage, invalidArg, notFound } = require('./errors');

const registry = new Map();

function def(name, spec) {
    const group = name.includes('.') ? name.split('.')[0] : 'misc';
    const entry = Object.assign({ name, group, args: {}, examples: [], needsEngine: false, needsProject: false }, spec);
    registry.set(name, entry);
    return entry;
}

function get(name) { return registry.get(name) || null; }
function list() { return [...registry.values()]; }

function groups() {
    const out = new Map();
    for (const cmd of registry.values()) {
        if (!out.has(cmd.group)) out.set(cmd.group, []);
        out.get(cmd.group).push(cmd);
    }
    for (const cmds of out.values()) cmds.sort((a, b) => a.name.localeCompare(b.name));
    return [...out.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([group, commands]) => ({ group, commands }));
}

function schema() {
    return {
        format: 'cairn-cli-schema',
        version: 1,
        envelope: { ok: 'boolean', command: 'string', result: 'object', error: '{code, message, details?}' },
        groups: groups().map(({ group, commands }) => ({
            group,
            commands: commands.map(c => ({
                name: c.name,
                group: c.group,
                summary: c.summary,
                args: Object.entries(c.args).map(([arg, spec]) => Object.assign({ name: arg }, spec)),
                returns: c.returns || null,
                needsProject: !!c.needsProject,
                notes: c.notes || null,
                examples: c.examples,
            })),
        })),
    };
}

function resolveName(name) {
    if (registry.has(name)) return registry.get(name);
    const matches = list().filter(c => c.name.endsWith('.' + name) || c.name === name);
    if (matches.length === 1) return matches[0];
    if (matches.length > 1) {
        throw usage(`ambiguous command '${name}'`, { candidates: matches.map(m => m.name) });
    }
    return null;
}

// --------------------------------------------------------------------- parse
function parseArgv(command, argv, ctx) {
    const specs = command.args || {};
    const order = Object.keys(specs);
    const raw = {};
    const positional = [];
    let i = 0;

    const setValue = (key, value) => {
        if (specs[key] && specs[key].type === 'string[]') raw[key] = (raw[key] || []).concat([value]);
        else raw[key] = value;
    };

    while (i < argv.length) {
        const token = argv[i++];
        if (token === '--') { positional.push(...argv.slice(i)); break; }
        if (token.startsWith('--')) {
            let key = token.slice(2);
            let value;
            const eq = key.indexOf('=');
            if (eq >= 0) { value = key.slice(eq + 1); key = key.slice(0, eq); }
            if (value === undefined && (key.startsWith('no-') || (specs[key] && specs[key].type === 'bool'))) {
                value = key.startsWith('no-') ? false : true;
                if (key.startsWith('no-')) key = key.slice(3);
            } else if (value === undefined) {
                if (!Object.prototype.hasOwnProperty.call(specs, key)) {
                    throw invalidArg(`unknown option '--${key}' for ${command.name}`, {
                        command: command.name,
                        expected: Object.keys(specs),
                        hint: 'global options such as --project/--format/--pretty must come before the command name',
                    });
                }
                value = argv[i++];
                if (value === undefined) throw invalidArg(`option --${key} expects a value`);
            }
            setValue(key, value);
        } else {
            positional.push(token);
        }
    }

    for (let p = 0; p < positional.length; ++p) {
        const key = order[p];
        if (!key) throw usage(`unexpected argument '${positional[p]}'`, { command: command.name, expected: order });
        setValue(key, positional[p]);
    }

    const out = {};
    for (const [key, spec] of Object.entries(specs)) {
        const value = raw[key];
        if (value === undefined) {
            if (spec.required) throw invalidArg(`missing required argument --${key}`, { command: command.name });
            if (spec.default !== undefined) out[key] = spec.default;
            continue;
        }
        out[key] = coerce(spec, value, ctx, key);
    }
    return out;
}

function coerce(spec, value, ctx, key) {
    const type = spec.type || 'string';
    switch (type) {
        case 'string':
            return String(value);
        case 'path': {
            const str = String(value);
            if (str.startsWith('@')) return readJsonArg(str);
            return path.resolve(ctx.cwd, str);
        }
        case 'asset': {
            const str = String(value);
            if (str.startsWith('@')) return readJsonArg(str);
            const project = ctx.tryProject();
            return path.resolve(project || ctx.cwd, str);
        }
        case 'int': {
            const n = Number(value);
            if (!Number.isFinite(n)) throw invalidArg(`--${key} expects a number, got '${value}'`);
            return Math.trunc(n);
        }
        case 'number': {
            const n = Number(value);
            if (!Number.isFinite(n)) throw invalidArg(`--${key} expects a number, got '${value}'`);
            return n;
        }
        case 'bool':
            if (typeof value === 'boolean') return value;
            if (/^(1|true|yes|on)$/i.test(value)) return true;
            if (/^(0|false|no|off)$/i.test(value)) return false;
            throw invalidArg(`--${key} expects a boolean, got '${value}'`);
        case 'json':
            return typeof value === 'string' ? readJsonArg(value, key) : value;
        case 'string[]':
            return Array.isArray(value) ? value.map(String) : [String(value)];
        case 'enum': {
            const str = String(value);
            if (spec.enum && !spec.enum.includes(str)) {
                throw invalidArg(`--${key} must be one of: ${spec.enum.join(', ')}`, { got: str });
            }
            return str;
        }
        case 'color': {
            const parsed = require(path.join(__dirname, '..', '..', 'editor', 'main', 'imaging.js')).parseColor(value);
            if (!parsed) throw invalidArg(`--${key} expects a color (hex, "r,g,b,a" or array), got '${value}'`);
            return parsed;
        }
        default:
            return value;
    }
}

// Same contract as parseArgv but for already-structured args (batch mode).
function parseArgObject(command, values, ctx) {
    const specs = command.args || {};
    if (Array.isArray(values)) return parseArgv(command, values.map(String), ctx);
    if (values === null || typeof values !== 'object') {
        throw invalidArg(`args for ${command.name} must be an object or an array`);
    }
    for (const key of Object.keys(values)) {
        if (!specs[key]) {
            throw invalidArg(`unknown argument '${key}' for ${command.name}`, {
                command: command.name,
                expected: Object.keys(specs),
            });
        }
    }
    const out = {};
    for (const [key, spec] of Object.entries(specs)) {
        const value = values[key];
        if (value === undefined || value === null) {
            if (spec.required) throw invalidArg(`missing required argument '${key}'`, { command: command.name });
            if (spec.default !== undefined) out[key] = spec.default;
            continue;
        }
        out[key] = coerce(spec, value, ctx, key);
    }
    return out;
}

function readJsonArg(str, key) {
    const text = str.startsWith('@')
        ? (str === '@-' ? fs.readFileSync(0, 'utf8') : fs.readFileSync(str.slice(1), 'utf8'))
        : str;
    try {
        return JSON.parse(text);
    } catch (err) {
        throw invalidArg(`--${key || 'value'} is not valid JSON: ${err.message}`);
    }
}

// Resolves "scene.load" style names, returns null when unknown.
function lookup(name) {
    if (!name) return null;
    return resolveName(String(name).trim());
}

function requireCommand(name) {
    const cmd = lookup(name);
    if (!cmd) {
        throw notFound(`unknown command '${name}'`, { hint: 'run `cairn engine.schema` for the full command list' });
    }
    return cmd;
}

module.exports = { def, get, list, groups, schema, lookup, requireCommand, parseArgv, parseArgObject, coerce };
