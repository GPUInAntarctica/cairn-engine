# Contributing to Cairn Engine

Thanks for your interest in improving Cairn! This document covers the basics; for architecture background read [docs/architecture.md](docs/architecture.md).

## Getting set up

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install
npm run build
npm test          # must be green before you start
```

## Project layout

```
engine/    C++ runtime (CMake target cairn_engine)
addon/     N-API binding (cairn_engine_addon.node)
editor/    Electron app (main/ = main process, renderer/ = UI)
runtime/   exported player shell
templates/ project templates (3D sandbox, 2D platformer)
test/      end-to-end smoke test
docs/      documentation (bilingual where it matters)
```

## Ground rules

1. **Engine stays dependency-free.** The C++ core may only grow vendored, single-file third-party code (like stb). Anything heavier belongs in the editor or is discussed first.
2. **The scripting API is a contract.** Changes to `engine/include/cairn/*.h` need a changelog entry and a test. Existing sample projects must keep compiling.
3. **Match the house style.** Engine: C++20, 4 spaces, `PascalCase` types / `camelCase` members, comments explain *why*. Editor: ES modules, no frameworks — plain DOM with small focused modules.
4. **Tests, please.** Engine changes go into `engine/tests/engine_test.cpp`; end-to-end behavior into `test/smoke.js`. The editor is screenshot-tested manually with the autotest harness (`CAIRN_AUTOTEST=1`).
5. **Keep the editor bilingual.** User-facing strings go through `t()` and exist in both `i18n/en.js` and `i18n/zh-CN.js`.

## Commit style

Short imperative subject (`Fix near-tangent grid line hang`), body explains the *why*. One logical change per commit.

## Pull requests

- Rebase onto `main`, keep diffs focused
- `npm test` green + the editor boots (`npm start`)
- Describe what changed and how to verify it
- Screenshots for UI changes are appreciated

## Reporting bugs

Open an issue with: Cairn version, OS + compiler, steps to reproduce, expected vs actual. For crashes, the console output is gold.
