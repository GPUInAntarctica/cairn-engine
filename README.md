<div align="center">

# Cairn Engine

**A compact C++ game engine with a focused editor for 2D and 3D projects.**

[![CI](https://github.com/cairn-engine/cairn-engine/actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Engine](https://img.shields.io/badge/engine-C%2B%2B20-00599C.svg)](docs/architecture.md)
[![Editor](https://img.shields.io/badge/editor-Electron-47848F.svg)](docs/editor-manual.md)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

Cairn is a compact game engine: a C++ runtime for rendering, physics and gameplay scripts, driven by an Electron editor for scene building and content creation. It keeps the dependency tree deliberately small — the engine core has **no third-party libraries** beyond two vendored stb headers, and the editor depends only on Electron, Monaco and esbuild.

Rendering is a **software rasterizer**: no GPU, no drivers, no system graphics API. It runs anywhere.

## Features

- **2D and 3D scenes** — a unified GameObject/Component model with hierarchy, duplication and readable `.cscene` JSON serialization
- **Software renderer** — multithreaded rasterization with GGX specular, cascaded shadow maps, sky gradient, fog, 2D sprites and a bitmap font
- **Post-processing** — bloom → color grading → vignette → FXAA, each independently switchable with a strict neutral passthrough
- **C++ scripting** — Unity-style components: `OnStart` / `OnUpdate` / `FixedUpdate` / trigger and collision callbacks, compiled to a shared library from the editor and hot-reloaded
- **The editor** — a dark, dense, keyboard-first workspace: hierarchy, reflection-driven inspector, viewport with select/move/rotate/scale and Play/Pause/Step, console, content browser
- **Code editor** — Monaco with C++ syntax highlighting, engine API completion and build diagnostics mapped onto lines
- **Image editor** — layers, brush/shape/fill tools, HSV picker and normal-map generation, shared with the CLI as a headless raster core
- **Command line** — `cairn` exposes every engine capability as structured JSON (scene/entity/component editing, headless play mode, script builds, screenshots, image editing) with a stdin batch mode, designed for AI agents to drive directly — **no MCP server, no daemon**
- **Bilingual UI** — full Simplified Chinese and English with instant switching

## Quick start

Requires Node.js ≥ 20, CMake ≥ 3.16 and a C++20 compiler (g++ ≥ 11 / clang++ / MSVC).

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install          # Electron / esbuild / Monaco / node-addon-api
npm run build        # CMake engine library + N-API addon + renderer bundle
npm test             # smoke test + CLI suite
npm start            # launch the editor
```

Create a project from the startup screen (3D sandbox or 2D platformer template) and press **Ctrl+P** to play.

## Command line

```bash
node cli/index.js engine.schema                  # self-describing command + argument schema
node cli/index.js --project ./game scene.tree    # read a scene
node cli/index.js --project ./game script.build  # compile and load gameplay scripts
node cli/index.js --project ./game exec.run --file steps.jsonl   # batch execution
```

Every command prints exactly one JSON document on stdout
(`{"ok":true,"command":...,"result":...}`), errors go to stderr as
`{"ok":false,"error":{"code","message","details"}}` with a non-zero exit code,
and `--format text` switches to a human-readable rendering. See
[docs/cli.md](docs/cli.md).

## Scripting example

```cpp
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnUpdate(float deltaTime) override {
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += PropFloat("speed") * deltaTime;   // inspector-editable field
        transform().SetEulerDegrees(euler);
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 90.0f);
    }
};

CAIRN_SCRIPT(Rotator)
```

The full scripting surface is in [docs/scripting-api.md](docs/scripting-api.md).

## Documentation

| Document | Contents |
| --- | --- |
| [**Technical manual**](docs/technical-manual.md) | **Complete reference**: coordinate system, render pipeline, PBR shading, shadow mapping, post chain, threading, data model, reflection, physics, script host, file formats, build system |
| [Architecture overview](docs/architecture.md) | Module breakdown, data flow, hot-reload mechanics |
| [Editor manual](docs/editor-manual.md) | Interface, keyboard navigation, workflows |
| [CLI reference](docs/cli.md) | `cairn` commands, JSON contract, batch mode |
| [Scripting API](docs/scripting-api.md) | Complete C++ scripting reference |
| [Building](docs/building.md) | Per-platform builds and troubleshooting |

## Project layout

```
engine/         C++ runtime — math, json, scene, renderer, physics, script host
addon/          N-API binding of the Engine facade (shared by editor and CLI)
editor/main/    Electron main process — window, dialogs, project I/O, script build
editor/renderer/Editor UI (plain DOM + ES modules, bundled with esbuild)
runtime/        Exported player — loads a project and runs it without the editor
cli/            `cairn` command line
templates/      New-project templates (3D sandbox, 2D platformer)
test/           smoke test + CLI suite
docs/           Documentation
```

## Status

Cairn is at version **0.1.0** — an early but usable foundation. See the
[CHANGELOG](CHANGELOG.md) for the milestone history, and
[CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

[MIT](LICENSE)