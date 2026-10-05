# AGENTS.md

Instructions for AI agents working in this repository — and for using Cairn itself
from the command line.

## Build and test

```bash
npm install
npm run build     # CMake engine library + N-API addon + esbuild renderer bundle
npm test          # engine unit tests + end-to-end smoke + CLI suite
```

The native addon (`build/cairn_engine_addon.node`) must exist before anything that
touches the engine runs; the CLI reports `engine_unavailable` if it is missing.

## Using the engine from the command line

`cairn` is the supported automation surface for the engine — no MCP server, no
daemon, no editor window. It lives in `cli/` and is described in
[docs/cli.md](docs/cli.md).

```bash
node cli/index.js engine.schema                                  # discover every command + argument schema
node cli/index.js --project ./game --scene Assets/Scenes/Main.cscene scene.tree
node cli/index.js --project ./game exec.run --file steps.jsonl   # multi-step batch in one session
```

Contract every command follows:

- exactly one JSON document on stdout: `{"ok":true,"command":"…","result":{…}}`
- errors on stderr as `{"ok":false,"command":"…","error":{"code","message","details"}}`
- exit codes: `0` success, `1` command failure, `2` usage error
- `--format text` for humans, `--pretty` / `--raw` to reshape the JSON

## Where things live

| Path | Contents |
| --- | --- |
| `engine/` | C++ runtime; public contract in `engine/include/cairn/` |
| `addon/` | N-API binding of the `Cairn::Engine` facade |
| `cli/lib/` | command registry, argument schema/coercion, JSON output, session state |
| `cli/commands/` | command definitions grouped by prefix (`scene.*`, `entity.*`, …) |
| `editor/main/` | main-process modules shared by the editor and the CLI |
| `editor/main/imaging.js` | headless raster core (layers, drawing, PNG/BMP, normal maps) |
| `editor/renderer/` | editor UI (ES modules, no framework) |
| `test/` | `smoke.js` (engine end-to-end), `cli.js` (CLI end-to-end) |
| `docs/` | architecture, editor manual, scripting API, building, CLI |

## Rules when changing the engine or the CLI

1. Engine C++ changes need a case in `engine/tests/engine_test.cpp`; behaviour that
   spans the addon and Node belongs in `test/smoke.js`; CLI behaviour in `test/cli.js`.
2. Anything exposed to scripts (`engine/include/cairn/*.h`) is a contract: changelog
   entry plus a test, and existing sample projects must keep compiling.
3. New CLI commands must declare a summary, an argument schema and an example, then
   run `npm run docs:cli` to regenerate the reference table in `docs/cli.md`.
4. Keep stdout a pure JSON channel in the CLI: engine logs are buffered and read with
   `runtime.logs` (echo is off unless `--log-echo`).
5. No new third-party dependencies in the engine core, and none in the CLI — PNG/BMP
   codecs live in `editor/main/imaging.js` by design.
6. Editor user-facing strings go through `t()` and exist in both `i18n/en.js` and
   `i18n/zh-CN.js`.
