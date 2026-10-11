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
  shaders/ GLSL for the Vulkan backend, compiled to SPIR-V at build time
  cmake/   build helpers (SPIR-V embedding)
addon/     N-API binding (cairn_engine_addon.node)
editor/    Electron app (main/ = main process, renderer/ = UI)
cli/       cairn CLI (command registry + JSON contract + batch mode)
runtime/   exported player shell
templates/ project templates (3D sandbox, 2D platformer)
test/      smoke test + CLI suite
scripts/   build + CLI doc generation
docs/      documentation (bilingual where it matters)
```

## Ground rules

1. **Engine stays dependency-free.** The C++ core may only grow vendored, single-file third-party code (like stb). Anything heavier belongs in the editor or is discussed first. The Vulkan backend is the exception that proves the rule: the loader is resolved at runtime with `dlopen`, so there is no link-time dependency and the engine still builds where Vulkan is absent.
2. **Two render backends, one picture.** GPU and CPU share the scene-gather front end (`GatherScene` / `FitCascadeSet`). The software rasterizer is the reference implementation and the test suite pins it, so a rendering difference between the two is a bug in the GPU path. Debug GPU work with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` — and note that validation being *clean* does not mean the frame is *correct*: a zeroed `colorWriteMask` or a float written where a shader reads an `int` both pass validation and both render nothing.
3. **The scripting API is a contract.** Changes to `engine/include/cairn/*.h` need a changelog entry and a test. Existing sample projects must keep compiling.
4. **Match the house style.** Engine: C++20, 4 spaces, `PascalCase` types / `camelCase` members, comments explain *why*. Editor: ES modules, no frameworks — plain DOM with small focused modules.
5. **Tests, please.** `npm test` runs the engine unit tests, the smoke test and the CLI suite; `npm run test:engine` runs just the C++ ones. Engine changes go into `engine/tests/engine_test.cpp`; end-to-end behavior into `test/smoke.js`; CLI behavior into `test/cli.js`. Both JS suites pin `CAIRN_RENDER_BACKEND=software`: they assert CPU-exact pixel maths, and every GPU frame blocks on a readback fence that would starve the desktop. New CLI commands need a summary, an argument schema and an example, and `npm run docs:cli` regenerates the reference in `docs/cli.md`. The editor is screenshot-tested manually with the autotest harness (`CAIRN_AUTOTEST=1`).
6. **Keep the editor bilingual.** User-facing strings go through `t()` and exist in both `i18n/en.js` and `i18n/zh-CN.js`.

## Commit style

Short imperative subject (`Fix near-tangent grid line hang`), body explains the *why*. One logical change per commit.

## Pull requests

- Rebase onto `main`, keep diffs focused
- `npm test` green + the editor boots (`npm start`)
- If you touched the renderer: `CAIRN_RENDER_BACKEND=vulkan npm run test:engine` green, and ideally clean under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
- Describe what changed and how to verify it
- Screenshots for UI changes are appreciated

## Reporting bugs

Open an issue with: Cairn version, OS + compiler, steps to reproduce, expected vs actual. For crashes, the console output is gold.
