# Cairn CLI

`cairn` exposes every engine capability — project and scene authoring, entity and
component editing, headless play mode, script compilation, screenshots and the
image editor — as a command line tool whose output is designed for AI agents to
read and write. It is a plain CLI: no MCP server, no daemon, no editor window.

```bash
npm run build            # engine + addon must be built first
node cli/index.js --help # or: npm run cairn -- --help
```

After `npm install` the binary is also linked as `node_modules/.bin/cairn`.

## Output contract

Every invocation writes **exactly one JSON document to stdout**:

```jsonc
// success
{"ok": true, "command": "entity.createPrimitive", "result": { "id": 7, "name": "Crate", "mesh": "Cube" }}
// failure (stderr, non-zero exit)
{"ok": false, "command": "component.set", "error": {"code": "prop_rejected", "message": "...", "details": {...}}}
```

| Exit code | Meaning |
| --- | --- |
| `0` | success |
| `1` | command failed (`not_found`, `prop_rejected`, `scene_load_failed`, …) |
| `2` | usage error (unknown command, bad or missing argument) |

Stable flags:

- `--format json` (default) / `--format text` — text renders tables and key/value blocks for humans.
- `--pretty` — indent the JSON.
- `--raw` — print only `result`, without the `{ok, command}` wrapper.
- `--verbose` — add a stack trace to errors.

Global options must appear **before** the command name so they never collide
with command arguments (`scene.json --pretty` is a command flag, `cairn --pretty
scene.json` is the global one):

```
cairn [--project DIR] [--scene FILE.cscene] [--image FILE.png]
      [--width N] [--height N] [--format json|text] [--pretty] [--raw]
      [--verbose] [--log-echo] <command> [arguments]
```

Value syntax: `--value '[1,2,3]'` is parsed as JSON, `--value @file.json` reads
from a file and `--value @-` reads from stdin. Booleans are `--flag` / `--no-flag`.

## Discovery

Agents should start with the schema, which is generated from the same registry
the dispatcher enforces:

```bash
cairn engine.schema                  # every command with its argument schema
cairn engine.schema --group entity   # one group
cairn engine.commands                # flat name + summary list
cairn engine.help entity.create      # detail for one command
cairn engine.capabilities            # primitives, component types, key codes
cairn component.schema               # engine reflection: every property per component
```

`component.schema` is the authoritative source for property names and types —
it probes a throwaway entity for every component type and restores the current
scene afterwards.

## Sessions

Each `cairn` process owns one session: a project root, an optional loaded scene,
a viewport size and one engine instance. Use globals to prime it:

```bash
cairn --project ./game --scene Assets/Scenes/Main.cscene scene.tree
```

State that has to survive across calls must be written to disk (`scene.save`,
`image.save`) — that is what makes the workflow reproducible.

### Picking a render backend

Rendering defaults to the Vulkan GPU backend, falling back to software when no
Vulkan 1.3 device is present. Two ways to steer it:

```bash
cairn --project ./game scene.settings.set --key renderBackend --value software   # auto | vulkan | software
cairn --project ./game scene.settings.set --key msaa --value 4                   # 1 | 2 | 4 | 8
CAIRN_RENDER_BACKEND=software cairn --project ./game exec.run --file steps.jsonl
```

The environment variable only speaks for scenes left on `auto`; a project that
names a backend keeps it. Prefer it for bulk work — a GPU frame ends in a
blocking readback, so a few hundred rendered frames will fight the desktop for
the card. `render.editorFrame` reports which backend actually ran in `stats`.

## Recipes

Read a scene:

```bash
cairn --project ./game --scene Assets/Scenes/Main.cscene scene.tree
cairn --project ./game --scene Assets/Scenes/Main.cscene scene.hierarchy
```

Author a scene. Single calls lose their session state, so anything with more
than two or three steps belongs in an `exec.run` batch:

```bash
cairn --project ./game scene.new --name Level
cairn --project ./game entity.createPrimitive --mesh Cube --name Floor --position '[0,-0.5,0]' --scale '[40,1,40]'
cairn --project ./game component.add --id 1 --type BoxCollider --props '{"size":[40,1,40]}'
cairn --project ./game scene.save --file Assets/Scenes/Level.cscene
```

One subtlety worth remembering: a batch whose first step is `scene.new` must
**not** be given `--scene <that file>`, because the session would try to load a
file the batch is about to create. Pass `--scene` only when loading something
that already exists. Working end-to-end examples live in the
[README](../README.md#driving-the-engine-from-an-agent).

Preview and simulate headlessly:

```bash
cairn --project ./game --scene Assets/Scenes/Level.cscene render.screenshot --out /tmp/level.png --width 1280 --height 720
cairn --project ./game script.build                 # compile + load gameplay scripts
cairn --project ./game runtime.play
cairn --project ./game runtime.simulate --seconds 5 --render
cairn --project ./game runtime.logs --level error
```

Frame order matters (`runtime.update` already does it for you):
`update(dt)` → `render*Frame()` → `endFrameInput()`. `runtime.play` snapshots
the scene so `runtime.stop` restores the pre-play state.

Textures and scripts:

```bash
cairn --project ./game assets.list --kind texture
cairn --project ./game script.build --compiler g++
cairn --project ./game script.classes
cairn --project ./game project.exportPlayer --target ./build/player
```

## Batch mode

`exec.run` reads a JSON array or JSONL from stdin (or `--file`) and runs every
step against **one** engine session, streaming one JSON object per step. This
is the efficient way for an agent to do multi-step work.

```jsonl
{"command":"scene.load","args":{"file":"Assets/Scenes/Main.cscene"},"id":"scene"}
{"command":"entity.createPrimitive","args":{"mesh":"Cube","name":"Crate","position":[0,2,0]},"id":"crate"}
{"command":"component.add","args":{"id":"$ref:crate","type":"RigidBody","props":{"mass":2}}}
{"command":"component.setProps","args":{"id":"$ref:crate","type":"MeshRenderer","props":{"albedoColor":[0.9,0.4,0.2,1]}}}
{"command":"runtime.play"}
{"command":"runtime.simulate","args":{"seconds":2}}
{"command":"runtime.stop"}
{"command":"scene.save","args":{"file":"Assets/Scenes/Playtest.cscene"}}
```

```bash
cairn --project ./game exec.run --file steps.jsonl
echo '{"command":"scene.new","args":{"name":"Test"}}' | cairn exec.run
```

- `id` on a step stores the step's result under that name; later steps reference it as `"$ref":"<id>"`.
- A step may carry `"session":{"project":"...","scene":"...","width":N,"height":N}` to reconfigure the session mid-batch.
- The batch stops at the first failure; pass `--no-stopOnError` to continue. Failures are also listed in the final summary object.

## Image editor

The image editor's operations are available headlessly and share their
implementation with the editor UI (`editor/main/imaging.js`), including PNG/BMP
codecs with no third-party dependency.

```bash
cairn --project ./game image.new --width 128 --height 128 --background transparent
cairn --project ./game image.draw.rect --x 8 --y 8 --width 64 --height 64 --filled --color '#3a7de0'
cairn --project ./game image.draw.brush --x 16 --y 16 --x2 100 --y2 100 --brush 12 --color '#e05a3a'
cairn --project ./game image.normalMap --strength 8 --out Assets/Textures/wall_n.png
cairn --project ./game image.save --out Assets/Textures/wall.png
```

Layer state lives in the process, so compose image work in one batch (or prime
the document with `--image FILE`). `image.open`, `image.layer.*`,
`image.draw.fill`, `image.replaceColor`, `image.flatten`, `image.resize`,
`image.pixel` and `image.info` (histogram, average color) round out the toolset.

## Command reference

<!-- BEGIN GENERATED COMMANDS -->

### `assets`

| Command | Arguments | Description |
| --- | --- | --- |
| `assets.assetsReload` | — | Drop cached textures and meshes so the next access re-reads from disk. |
| `assets.info` | `--asset` (string, required) | Check that an asset reference resolves, and report its size. |
| `assets.list` | `--kind` (enum, default "texture", one of texture \| model \| all) | List project assets (textures or models) as project-relative paths. |

### `component`

| Command | Arguments | Description |
| --- | --- | --- |
| `component.add` | `--id` (int, required)<br>`--type` (enum, required, one of Camera \| Light \| MeshRenderer \| SpriteRenderer \| TextRenderer \| RigidBody \| BoxCollider \| SphereCollider \| Collider2D \| Script)<br>`--props` (json) | Add a component to an entity (properties get their defaults). |
| `component.enable` | `--id` (int, required)<br>`--type` (string, required)<br>`--enabled` (bool, default true) | Enable or disable a component. |
| `component.get` | `--id` (int, required)<br>`--type` (string, required) | Read one component with all its property values and editor metadata. |
| `component.getProp` | `--id` (int, required)<br>`--type` (string, required)<br>`--prop` (string, required) | Read a single component property value. |
| `component.list` | `--id` (int)<br>`--type` (string) | List components of one entity, or of every entity in the scene. |
| `component.remove` | `--id` (int, required)<br>`--type` (string, required) | Remove a component from an entity. |
| `component.schema` | `--type` (string)<br>`--includeTransform` (bool, default true) | Reflection schema: every component type with its properties, kinds and defaults. |
| `component.set` | `--id` (int, required)<br>`--type` (string, required)<br>`--prop` (string, required)<br>`--value` (json, required) | Set a single component property. |
| `component.setProps` | `--id` (int, required)<br>`--type` (string, required)<br>`--props` (json, required) | Set several properties of one component in a single call. |

### `engine`

| Command | Arguments | Description |
| --- | --- | --- |
| `engine.capabilities` | — | Static engine vocabulary: primitives, component types, key codes, scene settings. |
| `engine.commands` | — | Flat list of command names and one-line summaries (compact discovery). |
| `engine.groups` | — | Command groups with their command names. |
| `engine.help` | `--command` (string) | Usage detail for a single command (text or JSON via --format). |
| `engine.info` | — | Report the current session: project, viewport, loaded scene and play state. |
| `engine.schema` | `--group` (string) | Machine-readable description of every command and argument schema. |
| `engine.version` | — | Print engine and CLI versions. |

### `entity`

| Command | Arguments | Description |
| --- | --- | --- |
| `entity.create` | `--name` (string, default "GameObject")<br>`--parent` (int, default 0)<br>`--position` (json) | Create an empty GameObject. |
| `entity.createPrimitive` | `--mesh` (enum, required, one of Cube \| Sphere \| Plane \| Quad \| Cylinder \| Cone \| Torus)<br>`--name` (string, default "GameObject")<br>`--parent` (int, default 0)<br>`--position` (json)<br>`--scale` (json) | Create a GameObject with a MeshRenderer using a built-in primitive mesh. |
| `entity.createSprite` | `--name` (string, default "Sprite")<br>`--parent` (int, default 0)<br>`--texture` (string)<br>`--position` (json)<br>`--color` (color) | Create a GameObject with a SpriteRenderer (2D). |
| `entity.destroy` | `--id` (int, required) | Destroy an entity and its children. |
| `entity.duplicate` | `--id` (int, required) | Duplicate an entity (deep copy including children). |
| `entity.get` | `--id` (int, required) | Full description of one entity: transform, components, properties and editor metadata. |
| `entity.highlight` | `--id` (int, default 0) | Set or clear the editor selection outline entity. |
| `entity.set` | `--id` (int, required)<br>`--name` (string)<br>`--active` (bool)<br>`--tag` (string)<br>`--parent` (int)<br>`--fields` (json) | Change entity name, active flag, tag, parent or several at once. |
| `entity.transform` | `--id` (int, required)<br>`--position` (json)<br>`--rotation` (json)<br>`--scale` (json)<br>`--move` (json) | Set local position, rotation (Euler degrees) and/or scale. |

### `exec`

| Command | Arguments | Description |
| --- | --- | --- |
| `exec.run` | `--file` (path)<br>`--stopOnError` (bool, default true) | Run a batch of commands from stdin (JSON array or JSONL) against one engine session. |

### `image`

| Command | Arguments | Description |
| --- | --- | --- |
| `image.color` | `--value` (color, required)<br>`--to` (enum, default "hex", one of hex \| rgb255 \| rgb01 \| hsv) | Convert colors between hex, rgb 0-255, rgb 0-1 and hsv. |
| `image.draw.brush` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--x` (number, required)<br>`--y` (number, required)<br>`--x2` (number)<br>`--y2` (number)<br>`--erase` (bool, default false) | Paint a brush stroke along a path (single point when no end is given). |
| `image.draw.ellipse` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--cx` (number, required)<br>`--cy` (number, required)<br>`--rx` (number, required)<br>`--ry` (number, required)<br>`--filled` (bool, default false) | Draw an ellipse outline or a filled ellipse. |
| `image.draw.fill` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--x` (number, required)<br>`--y` (number, required)<br>`--tolerance` (int, default 24) | Flood fill the contiguous region around a pixel (tolerance matched). |
| `image.draw.line` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--x1` (number, required)<br>`--y1` (number, required)<br>`--x2` (number, required)<br>`--y2` (number, required) | Draw a line segment. |
| `image.draw.rect` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--x` (number, required)<br>`--y` (number, required)<br>`--width` (number, required)<br>`--height` (number, required)<br>`--filled` (bool, default false) | Draw a rectangle outline or a filled rectangle. |
| `image.flatten` | — | Collapse all visible layers into a single layer. |
| `image.info` | — | Document dimensions, layer list and pixel statistics (composite histogram, average color). |
| `image.layer.add` | `--name` (string, default "Layer")<br>`--index` (int)<br>`--fill` (color) | Insert an empty layer above the given one (or on top). |
| `image.layer.delete` | `--index` (int, required) | Delete a layer (the last remaining layer cannot be removed). |
| `image.layer.duplicate` | `--index` (int, required) | Copy a layer and insert the copy above it. |
| `image.layer.list` | — | List layers with visibility and opacity. |
| `image.layer.mergeDown` | `--index` (int, required) | Composite a layer into the one below it (honouring opacity) and remove it. |
| `image.layer.move` | `--index` (int, required)<br>`--to` (int, required) | Reorder a layer within the stack (index 0 is the bottom). |
| `image.layer.set` | `--index` (int, required)<br>`--name` (string)<br>`--visible` (bool)<br>`--opacity` (number) | Rename a layer or change its visibility/opacity. |
| `image.new` | `--width` (int, default 256)<br>`--height` (int, default 256)<br>`--background` (string, default "white")<br>`--layers` (int, default 1) | Create a layered image document (one transparent or coloured base layer). |
| `image.normalMap` | `--source` (enum, default "luma", one of luma \| alpha)<br>`--strength` (number, default 6)<br>`--invertX` (bool, default false)<br>`--invertY` (bool, default false)<br>`--out` (string)<br>`--name` (string, default "Normal Map") | Generate a tangent-space normal map from the composite height field and add it as a new layer. |
| `image.open` | `--file` (string, required) | Load a PNG or BMP file as a single-layer document. |
| `image.pixel` | `--x` (int, required)<br>`--y` (int, required)<br>`--layer` (int) | Sample a pixel from the active layer or the composite. |
| `image.replaceColor` | `--layer` (int)<br>`--color` (color, default "#ffffff")<br>`--opacity` (number, default 1)<br>`--brush` (number, default 1)<br>`--hardness` (number, default 0.8)<br>`--from` (color, required)<br>`--tolerance` (int, default 8) | Recolor every pixel of a layer whose color matches within a tolerance. |
| `image.resize` | `--width` (int, required)<br>`--height` (int, required) | Resample every layer to new dimensions (nearest neighbour). |
| `image.save` | `--out` (string)<br>`--layer` (int) | Write the composited image to a PNG file. |

### `input`

| Command | Arguments | Description |
| --- | --- | --- |
| `input.endFrame` | — | Close the input frame: wasDown <- down (required so GetKeyDown/GetKeyUp fire once). |
| `input.keyDown` | `--key` (string, required) | Press a key (held until keyUp or end of the next frame). |
| `input.keyUp` | `--key` (string, required) | Release a key. |
| `input.mouseDown` | `--button` (string, default "left") | Press a mouse button (0 = left, 1 = middle, 2 = right). |
| `input.mouseMove` | `--x` (number, required)<br>`--y` (number, required) | Move the mouse cursor (origin is bottom-left, y grows upward). |
| `input.mouseUp` | `--button` (string, default "left") | Release a mouse button. |
| `input.tap` | `--key` (string, required)<br>`--hold` (number, default 1)<br>`--dt` (number, default 0.016666666666666666)<br>`--render` (bool, default false) | Press and release a key, then run one frame so GetKeyDown/GetKeyUp fire. |

### `project`

| Command | Arguments | Description |
| --- | --- | --- |
| `project.create` | `--name` (string, required)<br>`--parent` (path)<br>`--template` (string, default "project-3d") | Create a new project directory from a template (seeds Assets and starter textures). |
| `project.exportPlayer` | `--target` (path, required)<br>`--compiler` (string)<br>`--skipScripts` (bool, default false) | Build gameplay scripts and package a standalone Electron player directory. |
| `project.info` | — | Read project.cproj of the current project. |
| `project.list` | — | List recent projects from the preferences store. |
| `project.preferences` | `--set` (json) | Read or write editor preferences (language, compiler, recent projects). |
| `project.scenes` | — | List scene files in Assets/Scenes. |
| `project.scripts` | — | List gameplay script sources in Assets/Scripts. |
| `project.templates` | — | List available project templates. |

### `render`

| Command | Arguments | Description |
| --- | --- | --- |
| `render.editorFrame` | — | Render one frame through the editor camera and return draw statistics. |
| `render.gameFrame` | — | Render one frame through the scene MainCamera (falls back to the editor view when playing without one). |
| `render.pick` | `--x` (int, required)<br>`--y` (int, required) | Pick the entity under a viewport pixel (origin bottom-left). |
| `render.preview` | `--out` (path, required)<br>`--width` (int, default 960)<br>`--height` (int, default 540)<br>`--frames` (int, default 1) | Set the viewport, render N editor frames and save the last one (one-shot preview render). |
| `render.screenshot` | `--out` (path, required)<br>`--mode` (enum, default "editor", one of editor \| game)<br>`--width` (int)<br>`--height` (int) | Render a frame and write it to a PNG file. |
| `render.setViewport` | `--width` (int, default 960)<br>`--height` (int, default 540) | Set the render target size used by subsequent frames and screenshots. |
| `render.viewCamera.get` | — | Read the editor view camera (position, yaw, pitch, projection). |
| `render.viewCamera.reset` | — | Reset the editor view camera to its default framing. |
| `render.viewCamera.set` | `--position` (json)<br>`--yaw` (number)<br>`--pitch` (number)<br>`--ortho` (bool)<br>`--orthoSize` (number)<br>`--fov` (number) | Move the editor view camera (affects editor frames and screenshots). |
| `render.worldToScreen` | `--x` (number, required)<br>`--y` (number, required)<br>`--z` (number, required) | Project a world position into viewport pixels. |

### `runtime`

| Command | Arguments | Description |
| --- | --- | --- |
| `runtime.audio` | — | Drain queued audio events emitted by scripts. |
| `runtime.logEcho` | `--enabled` (bool, default true) | Enable or disable mirroring engine log lines onto stdout (off by default so stdout stays pure JSON). |
| `runtime.logs` | `--level` (enum, one of info \| warning \| error)<br>`--text` (string) | Drain the engine log ring buffer (id, level, text). |
| `runtime.pause` | — | Pause the simulation without leaving play mode. |
| `runtime.play` | — | Enter play mode (snapshots the scene so runtime.stop can restore it). |
| `runtime.resume` | — | Resume a paused simulation. |
| `runtime.simulate` | `--seconds` (number, default 1)<br>`--dt` (number, default 0.016666666666666666)<br>`--render` (bool, default false)<br>`--play` (bool, default false) | Run the simulation for a duration in seconds (convenience wrapper over runtime.update). |
| `runtime.state` | — | Report play/pause state and the current play-mode time. |
| `runtime.step` | — | Advance exactly one fixed step (1/60 s) while paused. |
| `runtime.stop` | — | Leave play mode and restore the pre-play scene snapshot. |
| `runtime.update` | `--dt` (number, default 0.016666666666666666)<br>`--frames` (int, default 1)<br>`--render` (bool, default false) | Advance the simulation: update -> optional render -> end-of-frame input. |

### `scene`

| Command | Arguments | Description |
| --- | --- | --- |
| `scene.apply` | `--json` (string)<br>`--file` (path) | Replace the current scene with a .cscene JSON document (undo/paste helper). |
| `scene.entities` | `--tag` (string) | Compact entity index: id, name, tag, parent, active, component types. |
| `scene.find` | `--name` (string)<br>`--tag` (string) | Find entities by name or tag. |
| `scene.hierarchy` | — | Scene tree nested by parent/child relationships. |
| `scene.json` | `--pretty` (bool, default true)<br>`--file` (path) | Serialize the scene to a .cscene JSON document. |
| `scene.load` | `--file` (string) | Load a .cscene file into the session. |
| `scene.new` | `--name` (string, default "Scene") | Create an empty in-memory scene. |
| `scene.save` | `--file` (string) | Write the loaded scene back to a .cscene file. |
| `scene.settings.get` | — | Read scene settings (gravity, ambient, background, fog, name). |
| `scene.settings.set` | `--key` (enum, required, one of name \| gravity \| ambient \| background \| fogEnabled \| fogColor \| fogDensity \| tonemap \| exposure \| shadows \| shadowResolution \| shadowDistance \| postVignette \| postVignetteRadius \| postVignetteSoftness \| postContrast \| postSaturation \| postBloomStrength \| postBloomThreshold \| postFxaa \| textureMemoryMB \| renderBackend \| msaa)<br>`--value` (json, required) | Set one scene setting by name. |
| `scene.snapshot` | `--file` (path) | Capture the scene as JSON for later restore (undo helper). |
| `scene.stats` | — | Renderer-side counters for the current scene (objects, mesh renderers, draw calls). |
| `scene.tree` | `--filter` (string)<br>`--component` (string) | Full scene tree: every entity with transform, components and properties. |

### `script`

| Command | Arguments | Description |
| --- | --- | --- |
| `script.build` | `--compiler` (string)<br>`--load` (bool, default true) | Compile Assets/Scripts into the gameplay shared library and return compiler diagnostics. |
| `script.classes` | — | List the script classes currently loaded in the engine session. |
| `script.compileCheck` | `--compiler` (string) | Syntax-check gameplay scripts without producing a loadable library. |
| `script.load` | `--path` (path) | Load a compiled gameplay library into the engine and list its script classes. |
| `script.output` | — | Report the path of the compiled gameplay library. |
| `script.sources` | — | List the C++ sources that script.build will compile. |

<!-- END GENERATED COMMANDS -->

Regenerate the table above with `npm run docs:cli` after adding commands.

## Notes and gotchas

- Engine logs are buffered, not echoed: read them with `runtime.logs` (which
  drains the ring buffer) so stdout stays a clean JSON channel. `--log-echo`
  restores the echoing the editor sees.
- Asset references in scenes and properties are **project-root relative**
  (`Assets/Textures/checker.png`); CLI paths may be absolute, relative to the
  cwd, or project relative — they are resolved for you.
- Enum-valued properties take integers; `component.schema` lists the option
  order (e.g. `mesh`: Cube=0 … Custom (OBJ)=7).
- Rotation is Euler **degrees** in `Transform.rotation`, matching the inspector.
- `EntityId` `0` means "invalid" / "nothing hit" (`render.pick` returns 0 when
  the ray misses).
- The engine is a process-wide singleton, so one CLI process = one engine.
