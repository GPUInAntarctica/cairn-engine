# Changelog

All notable changes to Cairn Engine are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows [SemVer](https://semver.org/).

## [0.1.0] - 2026-10-04

First public milestone: a complete, self-contained editor + runtime loop.

### Engine (C++20)
- GameObject/Component model with Transform hierarchy, reflection-driven property tables, `.cscene` JSON serialization
- Multithreaded software rasterizer: frustum clipping, perspective-correct interpolation, Blinn-Phong lighting (directional + point), sky gradient, fog, alpha-blended sprites with tangent-space normal-map lighting, bitmap font text, depth-tested debug lines
- Physics: semi-implicit Euler, box/sphere (3D) and XY box (2D) colliders, static/dynamic resolution with restitution, trigger events into script callbacks
- Scripting host: `dlopen`-based hot reload with per-load library copies, class registry via `CAIRN_SCRIPT`, inspector metadata via `DescribeProps`
- Procedural primitives (cube, sphere, plane, quad, cylinder, cone, torus), OBJ import, stb-based PNG/JPG/BMP textures with runtime generator fallbacks
- Editor-grade engine facade: picking, world/screen projection, play-state snapshots, input injection, log ring buffer, audio event queue

### Native addon
- N-API (node-addon-api) binding, ABI-stable across Electron releases; frame delivered as a copied buffer (Electron renderers disallow external buffers)

### Editor (Electron)
- Project browser, project templates (3D sandbox, 2D platformer), recent projects, preferences with persisted language/compiler/behavior
- Unreal-flavored dark UI: hierarchy (drag-free tree, context menus, inline rename), reflection-driven inspector with drag-scrub fields, asset pickers and enum dropdowns, console with filters, content browser with thumbnails and asset operations
- Viewport: orbit/pan/fly camera, click picking, move/rotate/scale tools, play/pause/step/stop with scene restore, editor grid and selection outline
- Monaco code editor: C++ highlighting, engine API completion, build diagnostics mapped to lines; scripts build via the system compiler and hot-reload
- Image editor: layers, brush/eraser/fill/line/rect/ellipse/eyedropper, HSV picker with swatches, undo/redo, PNG save/export, height→normal-map generation
- Bilingual UI (Simplified Chinese / English) with instant switching

### Tooling
- CMake engine build, scripted addon build with rpath handling, esbuild renderer bundle
- Native engine test suite (math, JSON, scene round-trip, rendering, physics settle) and Node end-to-end smoke test (script compile, hot reload, physics+script interplay, PNG capture)
- GitHub Actions CI, MIT license, project documentation
