// Cairn Engine - regenerates the command reference section in docs/cli.md.
//
//   node scripts/gen-cli-docs.js
//
// The CLI command table is derived from the registry so the docs cannot drift
// from the implementation.
'use strict';

const fs = require('fs');
const path = require('path');

require('../cli/index.js');

const registry = require('../cli/lib/registry');
const docPath = path.join(__dirname, '..', 'docs', 'cli.md');
const BEGIN = '<!-- BEGIN GENERATED COMMANDS -->';
const END = '<!-- END GENERATED COMMANDS -->';

function typeLabel(spec) {
    const bits = [spec.type || 'string'];
    if (spec.required) bits.push('required');
    else if (spec.default !== undefined) bits.push(`default ${JSON.stringify(spec.default)}`);
    if (spec.enum) bits.push(`one of ${spec.enum.join(' \\| ')}`);
    return bits.join(', ');
}

function render() {
    const lines = [];
    for (const { group, commands } of registry.groups()) {
        lines.push(`### \`${group}\``, '');
        lines.push('| Command | Arguments | Description |');
        lines.push('| --- | --- | --- |');
        for (const cmd of commands) {
            const args = Object.entries(cmd.args)
                .map(([name, spec]) => `\`--${name}\` (${typeLabel(spec)})`)
                .join('<br>') || '—';
            lines.push(`| \`${cmd.name}\` | ${args} | ${cmd.summary} |`);
        }
        lines.push('');
    }
    return lines.join('\n');
}
const doc = fs.readFileSync(docPath, 'utf8');
const start = doc.indexOf(BEGIN);
const end = doc.indexOf(END);
if (start < 0 || end < 0) {
    console.error(`[cairn] ${docPath} is missing the generated-section markers`);
    process.exit(1);
}
const next = doc.slice(0, start + BEGIN.length) + '\n\n' + render() + '\n' + doc.slice(end);
fs.writeFileSync(docPath, next);
console.log(`[cairn] docs -> ${path.relative(path.join(__dirname, '..'), docPath)} (${registry.list().length} commands)`);
