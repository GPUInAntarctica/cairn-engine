// Cairn CLI - result rendering.
//
// Every invocation prints exactly one JSON document on stdout (the agent
// contract) or a human-oriented text rendering with --format text. Errors go
// through the same envelope so a caller never has to parse prose.
'use strict';

const { CliError, EXIT_ERROR, EXIT_USAGE } = require('./errors');

function envelope(command, result) {
    return { ok: true, command, result: result === undefined ? null : result };
}

function success(command, result, opts = {}) {
    const payload = opts.raw ? (result === undefined ? null : result) : envelope(command, result);
    if (opts.format === 'text') {
        process.stdout.write(renderText(payload) + '\n');
    } else {
        process.stdout.write(JSON.stringify(payload, replacer, opts.pretty ? 2 : 0) + '\n');
    }
    return 0;
}

function failure(command, err, opts = {}) {
    const payload = err instanceof CliError
        ? Object.assign({ command }, err.toJSON())
        : { ok: false, command, error: { code: 'internal', message: String(err && err.message || err) } };
    if (opts.verbose && err && err.stack) payload.error.stack = err.stack;
    if (opts.format === 'text') {
        process.stderr.write(renderText(payload) + '\n');
    } else {
        process.stderr.write(JSON.stringify(payload, replacer, opts.pretty ? 2 : 0) + '\n');
    }
    if (err instanceof CliError) return err.exitCode;
    return EXIT_ERROR;
}

// Buffers and typed arrays are common payloads (framebuffer dumps); render them
// as a short descriptor instead of a wall of numbers.
function replacer(key, value) {
    if (Buffer.isBuffer(value)) return { $binary: 'buffer', bytes: value.length };
    if (ArrayBuffer.isView(value) && !(value instanceof DataView)) {
        return { $binary: 'typed-array', type: value.constructor.name, length: value.length };
    }
    return value;
}

function isPrimitive(v) {
    return v === null || ['string', 'number', 'boolean'].includes(typeof v);
}

function renderText(value, indent = '') {
    if (value === null || value === undefined) return 'null';
    if (Buffer.isBuffer(value)) return `<buffer ${value.length} bytes>`;
    if (ArrayBuffer.isView(value)) return `<${value.constructor.name} ${value.length}>`;
    if (typeof value === 'object') {
        if (value.$binary) return `<${value.$binary}${value.type ? ' ' + value.type : ''} ${value.bytes ?? value.length}>`;
        if (Array.isArray(value)) return renderArray(value, indent);
        const entries = Object.entries(value).filter(([, v]) => v !== undefined);
        if (entries.length === 0) return indent + '{}';
        return entries.map(([k, v]) => {
            if (isPrimitive(v) || v === null) return `${indent}${k}: ${v === null ? 'null' : v}`;
            const nested = renderText(v, indent + '  ');
            return `${indent}${k}:\n${nested}`;
        }).join('\n');
    }
    return indent + String(value);
}

function renderArray(items, indent) {
    if (items.length === 0) return indent + '(empty)';
    if (items.every(isPrimitive)) {
        const single = items.map(v => JSON.stringify(v)).join(', ');
        if (single.length + indent.length <= 100) return indent + single;
        return items.map(v => `${indent}${JSON.stringify(v)}`).join('\n');
    }
    const flat = items.every(item => item && typeof item === 'object' && !Array.isArray(item)
        && Object.values(item).every(isPrimitive));
    if (flat) return renderTable(items, indent);
    return items.map((item, i) => {
        const body = renderText(item, indent + '  ');
        const [head, ...rest] = body.split('\n');
        const label = items.length === 1 ? head : `${i}: ${head}`;
        return `${indent}- ${label}\n${rest.join('\n')}`.replace(/\n$/, '');
    }).join('\n');
}

function renderTable(items, indent) {
    const columns = [...new Set(items.flatMap(item => Object.keys(item)))];
    const cells = items.map(item => columns.map(c => {
        const v = item[c];
        return v === undefined ? '' : (typeof v === 'string' ? v : JSON.stringify(v));
    }));
    const widths = columns.map((c, i) => Math.max(c.length, ...cells.map(row => String(row[i]).length)));
    const line = (values) => indent + values
        .map((v, i) => String(v).padEnd(widths[i]))
        .join('  ').replace(/\s+$/, '');
    return [line(columns), line(widths.map(w => '-'.repeat(w))), ...cells.map(row => line(row))].join('\n');
}

module.exports = { envelope, success, failure, renderText, replacer, EXIT_USAGE, EXIT_ERROR };
