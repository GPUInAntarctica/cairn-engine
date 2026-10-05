// Cairn CLI - error types with stable machine-readable codes.
'use strict';

const EXIT_OK = 0;
const EXIT_ERROR = 1;
const EXIT_USAGE = 2;

class CliError extends Error {
    constructor(code, message, details) {
        super(message);
        this.name = 'CliError';
        this.code = code;
        this.details = details;
        this.exitCode = code === 'usage' || code === 'invalid_arg' ? EXIT_USAGE : EXIT_ERROR;
    }

    toJSON() {
        const error = { code: this.code, message: this.message };
        if (this.details !== undefined) error.details = this.details;
        return { ok: false, error };
    }
}

function usage(message, details) { return new CliError('usage', message, details); }
function invalidArg(message, details) { return new CliError('invalid_arg', message, details); }
function notFound(message, details) { return new CliError('not_found', message, details); }
function failed(code, message, details) { return new CliError(code, message, details); }

// Wraps an engine/addon {ok:false, error} result into a CliError.
function fromEngineResult(code, result, what) {
    const message = result && result.error ? result.error : `${what} failed`;
    return new CliError(code, message);
}

module.exports = { CliError, EXIT_OK, EXIT_ERROR, EXIT_USAGE, usage, invalidArg, notFound, failed, fromEngineResult };
