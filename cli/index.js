#!/usr/bin/env node

// Cairn Engine CLI - command line entry point.
//
//   cairn <command> [args] [global options]
//
// Every invocation prints one JSON document on stdout (agent contract) or a
// text rendering with --format text. Errors are printed on stderr as
// {ok:false, error:{code, message}} with exit code 1 (2 for usage errors).
'use strict';

const { Session } = require('./lib/session');
const registry = require('./lib/registry');
const { dispatch } = require('./lib/dispatch');
const { success, failure, renderText } = require('./lib/output');
const { CliError, usage } = require('./lib/errors');

// Loading the command modules registers every command in the registry.
require('./commands/core');
require('./commands/project');
require('./commands/scene');
require('./commands/entity');
require('./commands/component');
require('./commands/prefab');
require('./commands/physics');
require('./commands/runtime');
require('./commands/render');
require('./commands/script');
require('./commands/image');

const GLOBAL_USAGE = `cairn <command> [args] [global options]

Commands are grouped: engine, project, assets, scene, entity, component,
prefab, physics, runtime, input, render, script, image, exec.

Global options
  --project <dir>     project directory (default: $CAIRN_PROJECT or the cwd)
  --scene <file>      .cscene file to load when the engine starts
  --image <file>      PNG/BMP to load into the image editor session
  --width <n>         viewport width (default 960)
  --height <n>        viewport height (default 540)
  --format <fmt>      json (default) or text
  --pretty            indent JSON output
  --raw               print the result without the {ok, command} envelope
  --verbose           include stack traces in errors
  --log-echo          mirror engine logs onto stdout (off by default)
  -h, --help          show this help
  -v, --version       print versions

Global options must come before the command name; everything after the command
belongs to that command.

Value syntax
  --value '[1,2,3]'   JSON values for vectors, colors and objects
  --value @file.json  read the JSON from a file
  --value @-          read the JSON from stdin
  --flag / --no-flag  booleans take no value

Examples
  cairn engine.schema --group entity
  cairn --project ./game scene.load --file Assets/Scenes/Main.cscene
  cairn entity.createPrimitive --mesh Cube --name Floor --position '[0,-0.5,0]'
  cairn --scene Assets/Scenes/Main.cscene render.screenshot --out /tmp/shot.png
  cairn --project ./game exec.run < steps.jsonl
`;

const KNOWN_GLOBALS = new Set(['project', 'scene', 'image', 'width', 'height', 'format', 'pretty', 'raw', 'verbose', 'log-echo', 'help', 'version']);

// Global options are only recognized before the command name so that command
// arguments (scene.json --pretty, image.new --width) never collide with them.
function splitGlobals(argv) {
    const rest = [];
    const globals = { format: 'json' };
    let i = 0;
    for (; i < argv.length; ++i) {
        const token = argv[i];
        if (token === '--') { i++; break; }
        if (token === '-h') { globals.help = true; continue; }
        if (token === '--help') { globals.help = true; continue; }
        if (token === '-v' || token === '--version') { globals.version = true; continue; }
        if (!token.startsWith('--')) break;
        let key = token.slice(2);
        let value;
        const eq = key.indexOf('=');
        if (eq >= 0) { value = key.slice(eq + 1); key = key.slice(0, eq); }
        if (!KNOWN_GLOBALS.has(key)) break;
        if (value === undefined) {
            if (key === 'pretty' || key === 'raw' || key === 'verbose') { globals[key] = true; continue; }
            value = argv[++i];
            if (value === undefined) throw usage(`option --${key} expects a value`);
        }
        if (key === 'width' || key === 'height') globals[key] = Number(value);
        else if (key === 'pretty' || key === 'raw' || key === 'verbose') globals[key] = value !== 'false';
        else globals[key] = value;
    }
    for (; i < argv.length; ++i) rest.push(argv[i]);
    if (globals.format !== 'json' && globals.format !== 'text') {
        throw usage(`unknown --format '${globals.format}'`, { supported: ['json', 'text'] });
    }
    return { rest, globals };
}

function parseCommandLine(argv) {
    const { rest, globals } = splitGlobals(argv);
    if (rest.length === 0) {
        if (globals.version) return { name: 'engine.version', args: [], globals };
        return { name: null, args: {}, globals, help: true };
    }
    // "<group> <command>" is shorthand for "<group>.<command>".
    let name = rest[0];
    let args = rest.slice(1);
    if (!registry.get(name) && args.length && registry.get(name + '.' + args[0])) {
        name = name + '.' + args[0];
        args = args.slice(1);
    }
    return { name, args, globals };
}

async function main(argv) {
    let globals = { format: 'json' };
    let name = null;
    let argTokens = [];
    try {
        const parsed = parseCommandLine(argv);
        globals = parsed.globals;
        name = parsed.name;
        argTokens = parsed.args;
    } catch (err) {
        return failure(null, err, globals);
    }

    if (globals.help) {
        if (!name) {
            process.stdout.write(GLOBAL_USAGE);
            return 0;
        }
        argTokens = [name];
        name = 'engine.help';
    }
    if (!name) {
        process.stdout.write(GLOBAL_USAGE);
        return 0;
    }

    const session = new Session({
        cwd: process.cwd(),
        project: globals.project,
        scene: globals.scene,
        width: globals.width || 960,
        height: globals.height || 540,
        logEcho: globals['log-echo'] === true || globals['log-echo'] === 'true',
    });
    if (globals.image) session.loadImage(globals.image);
    const ctx = {
        cwd: process.cwd(),
        opts: globals,
        session,
        format: globals.format,
        pretty: !!globals.pretty,
        raw: !!globals.raw,
        verbose: !!globals.verbose,
        refs: Object.create(null),
        tryProject: () => session.tryProject(),
    };

    try {
        const command = registry.requireCommand(name);
        const { result } = await dispatch(command.name, argTokens, ctx, command);
        const code = success(command.name, result, globals);
        return ctx.exitCode || code;
    } catch (err) {
        return failure(name, err, globals);
    }
}

if (require.main === module) {
    main(process.argv.slice(2)).then(code => {
        process.exitCode = code;
    }).catch(err => {
        if (!(err instanceof CliError)) console.error(err);
        process.exitCode = 1;
    });
}

module.exports = { main, GLOBAL_USAGE, renderText };
