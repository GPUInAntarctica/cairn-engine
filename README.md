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

Frames render through a **Vulkan 1.3 backend** (dynamic rendering, loaded at runtime — no link-time dependency on the loader) with a **multithreaded software rasterizer** as the fallback. The same draw list feeds both, so a machine without a usable GPU still runs the engine, a scene file renders identically either way, and the backend is a scene setting rather than a build flag.

## Features

- **2D and 3D scenes** — a unified GameObject/Component model with hierarchy, duplication and readable `.cscene` JSON serialization
- **Two render backends, one picture** — a Vulkan GPU backend (meshes, sprites, text, debug lines, sky, cascaded shadow maps, bloom → grading → vignette → FXAA, optional 4×/8× MSAA) and a software rasterizer, sharing one scene-gather front end
- **Shading** — GGX specular with metallic/roughness, normal mapping, emissive, per-cascade shadow bias, sky gradient, fog, tone mapping (none/Reinhard/ACES/filmic)
- **C++ scripting** — Unity-style components: `OnStart` / `OnUpdate` / `FixedUpdate` / trigger and collision callbacks, compiled to a shared library from the editor or the CLI, and hot-reloaded
- **The editor** — a dark, dense, keyboard-first workspace: hierarchy, reflection-driven inspector, viewport with select/move/rotate/scale and Play/Pause/Step, console, content browser
- **Code editor** — Monaco with C++ syntax highlighting, engine API completion and build diagnostics mapped onto lines
- **Image editor** — layers, brush/shape/fill tools, HSV picker and normal-map generation, shared with the CLI as a headless raster core
- **Command line** — `cairn` exposes every engine capability as structured JSON (scene/entity/component editing, headless play mode, script builds, screenshots, image editing) with a batch mode, designed for AI agents to drive directly — **no MCP server, no daemon**
- **Bilingual UI** — full Simplified Chinese and English with instant switching

## Quick start

Requires Node.js ≥ 20, CMake ≥ 3.16 and a C++20 compiler (g++ ≥ 11 / clang++ / MSVC). Vulkan headers and `glslc` are optional: without them the engine still builds and renders in software.

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install          # Electron / esbuild / Monaco / node-addon-api
npm run build        # CMake engine library + N-API addon + renderer bundle
npm test             # engine unit tests + smoke test + CLI suite
npm start            # launch the editor
```

Create a project from the startup screen (3D sandbox or 2D platformer template) and press **Ctrl+P** to play. See [docs/editor-manual.md](docs/editor-manual.md) for the interface and [docs/building.md](docs/building.md) for per-platform notes and troubleshooting.

## Using the engine

Three surfaces, one engine. Pick by who is driving:

| Surface | Use it when | Entry point |
| --- | --- | --- |
| **Editor** | a human is building the game | `npm start` → [editor manual](docs/editor-manual.md) |
| **C++ scripts** | gameplay logic | `Assets/Scripts/*.cpp` → [scripting API](docs/scripting-api.md) |
| **`cairn` CLI** | an agent, a CI job, or a script | `node cli/index.js` → [CLI reference](docs/cli.md) |

A minimal gameplay script:

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

Compile it with **Ctrl+B** in the editor, or `cairn script.build` from the command line.

### Rendering backends

The backend is a scene setting, not a build flag:

```bash
cairn --project ./game scene.settings.set --key renderBackend --value vulkan   # or: auto, software
cairn --project ./game scene.settings.set --key msaa --value 4                # 1, 2, 4 or 8
```

`auto` (the default) uses the GPU when a Vulkan 1.3 device is present and silently falls back to software. `CAIRN_RENDER_BACKEND=auto|vulkan|software` overrides that default for the whole process — it only speaks for scenes left on `auto`, so a project that names a backend still gets it.

## Driving the engine from an agent

`cairn` is a plain command line tool: one JSON document per invocation on stdout, errors on stderr, exit codes 0/1/2. No daemon to start, no session to babysit, nothing to keep alive between steps.

```bash
node cli/index.js engine.schema        # every command with its argument schema — start here
node cli/index.js component.schema     # every component property, name, type and default
```

The full contract is in [docs/cli.md](docs/cli.md). The short version:

```bash
cairn --project ./game --scene Assets/Scenes/Main.cscene scene.tree
cairn --project ./game scene.settings.set --key renderBackend --value vulkan
cairn --project ./game script.build                  # compile Assets/Scripts/*.cpp
cairn --project ./game exec.run --file steps.jsonl   # multi-step batch, one session
```

**Work in batches.** Each process is a fresh session, so anything that must survive between steps goes through `scene.save` / `image.save` — or you run the whole sequence in one `exec.run`, which keeps one engine instance alive:

```jsonl
{"command":"entity.createPrimitive","args":{"mesh":"Cube","name":"Crate","position":[0,2,0]},"id":"crate"}
{"command":"component.add","args":{"id":"$ref:crate","type":"RigidBody","props":{"mass":2}}}
{"command":"component.setProps","args":{"id":"$ref:crate","type":"MeshRenderer","props":{"albedoColor":[0.9,0.4,0.2,1]}}}
{"command":"scene.save","args":{"file":"Assets/Scenes/Level1.cscene"}}
```

`"id"` stores a step's result; later steps refer to it as `"$ref":"<id>"`. A bad reference fails loudly and lists the ids that do exist.

### A complete session

Build a small level headlessly, script it, simulate it, and look at the result:

```bash
cairn --project ./game project.create --name OrbRun      # 3D sandbox or 2D platformer template
#   ... write Assets/Scripts/Collector.cpp ...
cairn --project ./game script.build                      # compile the gameplay C++
cairn --project ./game exec.run --file level.jsonl       # build the level (no --scene: it starts with scene.new)
cairn --project ./game --scene Assets/Scenes/Level1.cscene exec.run --file play.jsonl
cairn --project ./game project.exportPlayer --target ./player
```

The script has to exist and compile before the batch that attaches it, so the order above is the one to keep.

`Assets/Scripts/Collector.cpp` — the orbs spin, and vanish on contact:

```cpp
#include <cairn/cairn.h>
using namespace Cairn;

class Collector : public Component {
public:
    void OnUpdate(float dt) override {
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += PropFloat("spin") * dt;
        transform().SetEulerDegrees(euler);
    }

    void OnTriggerEnter(Component* other) override {
        Debug::Log("collected %s", gameObject().name.c_str());
        Gameplay::Destroy(&gameObject());
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("spin", PropType::Float, 120.0f);
    }
};

CAIRN_SCRIPT(Collector)
```

`level.jsonl` — floor, player, a collectible with its script, and a light:

```jsonl
{"command":"scene.new","args":{"name":"Level1"}}
{"command":"entity.createPrimitive","args":{"mesh":"Plane","name":"Floor","position":[0,0,0],"scale":[40,1,40]},"id":"floor"}
{"command":"component.setProps","args":{"id":"$ref:floor","type":"MeshRenderer","props":{"albedoColor":[0.35,0.38,0.42,1]}}}
{"command":"component.add","args":{"id":"$ref:floor","type":"BoxCollider","props":{"size":[40,1,40]}}}
{"command":"entity.createPrimitive","args":{"mesh":"Cube","name":"Player","position":[0,1,0]},"id":"player"}
{"command":"component.add","args":{"id":"$ref:player","type":"RigidBody","props":{"mass":1}}}
{"command":"component.add","args":{"id":"$ref:player","type":"BoxCollider","props":{"size":[1,1,1]}}}
{"command":"entity.createPrimitive","args":{"mesh":"Sphere","name":"Orb","position":[4,1,0]},"id":"orb"}
{"command":"component.add","args":{"id":"$ref:orb","type":"SphereCollider","props":{"radius":0.6,"isTrigger":true}}}
{"command":"component.add","args":{"id":"$ref:orb","type":"Script","props":{"class":"Collector","spin":150}}}
{"command":"entity.create","args":{"name":"Sun"},"id":"sun"}
{"command":"component.add","args":{"id":"$ref:sun","type":"Light","props":{"intensity":1.4}}}
{"command":"entity.transform","args":{"id":"$ref:sun","position":[6,8,4]}}
{"command":"scene.save","args":{"file":"Assets/Scenes/Level1.cscene"}}
```

`play.jsonl` — run the level and capture evidence. No ids needed: everything is already in the saved scene.

```jsonl
{"command":"runtime.play"}
{"command":"runtime.simulate","args":{"seconds":1}}
{"command":"runtime.logs","args":{"level":"info"}}
{"command":"render.screenshot","args":{"out":"/tmp/level.png","width":640,"height":400}}
{"command":"runtime.stop"}
```

`runtime.play` snapshots the scene so `runtime.stop` restores it; `runtime.simulate` advances fixed steps without a window. A screenshot is how you check your own work — read the PNG back before claiming the level is finished.

### Gotchas that cost an agent a cycle

- **Global flags come before the command name.** `cairn --project ./game --format text scene.tree`, not `cairn scene.tree --format text`.
- **`--value` is JSON.** Use `'[0,-0.5,0]'` for vectors, `'#3a7de0'` quoted for colors where a string is accepted. `tonemap` and `renderBackend` also accept bare names (`aces`, `vulkan`).
- **Primitive names are capitalized**: `Cube`, `Sphere`, `Plane`, `Quad`, `Cylinder`, `Cone`, `Torus`.
- **`component.setProps` needs the component to exist.** Add it first with `component.add`.
- **Pin the backend for bulk work.** Hundreds of GPU frames each block on a readback fence; `CAIRN_RENDER_BACKEND=software` keeps a batch off the GPU.
- **Find the bottleneck before guessing.** `cairn engine.profile` returns per-phase timings (`update`/`scripts`/`physics`, `render`/`gather`/`raster`/`post`/`gpu`) sorted heaviest-first, plus render counters and worker-pool telemetry. `CAIRN_JOBS_WORKERS=1` forces the CPU passes to run serially so a parallel speedup can be measured.
- **Check the exit code, not just stdout.** A failed command still prints JSON, with `"ok": false` and a `code` such as `invalid_arg` or `prop_rejected`.

## Documentation

| Document | Contents |
| --- | --- |
| [Architecture overview](docs/architecture.md) | Module breakdown, render backends, data flow, hot-reload mechanics |
| [Roadmap & progress](docs/roadmap.md) | Phase plan, delivered increments, what is next, known limits |
| [Editor manual](docs/editor-manual.md) | Interface, keyboard navigation, workflows |
| [CLI reference](docs/cli.md) | Every `cairn` command, the JSON contract, batch mode, recipes |
| [Scripting API](docs/scripting-api.md) | Complete C++ scripting reference |
| [Building](docs/building.md) | Requirements, GPU backend build options, per-platform notes, troubleshooting |

## Project layout

```
engine/         C++ runtime — math, json, scene, renderer, physics, script host
engine/shaders/ GLSL for the Vulkan backend (compiled to SPIR-V at build time)
addon/          N-API binding of the Engine facade (shared by editor and CLI)
editor/main/    Electron main process — window, dialogs, project I/O, script build
editor/renderer/ Editor UI (plain DOM + ES modules, bundled with esbuild)
runtime/        Exported player — loads a project and runs it without the editor
cli/            `cairn` command line
templates/      New-project templates (3D sandbox, 2D platformer)
test/           smoke test + CLI suite
docs/           Documentation
```

## Status

Cairn is at version **0.1.13** — an early but usable foundation. The GPU backend is the default but young; the software rasterizer remains the reference implementation and is what the test suite pins, so any rendering difference between the two is a bug in the GPU path rather than an intentional difference. See the [CHANGELOG](CHANGELOG.md) for the milestone history, and [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

[MIT](LICENSE)
