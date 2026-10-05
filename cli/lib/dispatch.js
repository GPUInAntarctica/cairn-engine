// Cairn CLI - dispatcher shared by argv mode and batch mode.
'use strict';

const { requireCommand, parseArgv, parseArgObject } = require('./registry');

async function dispatch(name, argsInput, ctx, known) {
    const command = known || requireCommand(name);
    const args = argsInput === undefined || argsInput === null
        ? parseArgv(command, [], ctx)
        : Array.isArray(argsInput)
            ? parseArgv(command, argsInput.map(String), ctx)
            : parseArgObject(command, argsInput, ctx);
    return { command, result: await command.run(ctx, args) };
}

module.exports = { dispatch };
