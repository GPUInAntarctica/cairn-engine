<div align="center">

# Cairn Engine

**一个精巧的游戏引擎，配套专注的 2D / 3D 编辑器。**

[![CI](https://github.com/cairn-engine/cairn-engine/actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Engine](https://img.shields.io/badge/engine-C%2B%2B20-00599C.svg)](docs/architecture.md)
[![Editor](https://img.shields.io/badge/editor-Electron-47848F.svg)](docs/editor-manual.md)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

Cairn 是一个精巧的游戏引擎：C++ 运行时负责渲染、物理与玩法脚本，Electron 编辑器负责场景搭建与内容创作。它刻意保持很小的依赖树——引擎核心**零第三方库**（仅 vendored 两个 stb 头文件），编辑器只依赖 Electron、Monaco 与 esbuild。

帧的渲染走 **Vulkan 1.3 后端**（dynamic rendering，运行时动态加载，不在链接期依赖 loader），并以**多线程软件光栅化**作为兜底。两条路径共用同一份 draw list，所以没有可用显卡的机器依然能跑，同一个场景文件在两种后端下画面一致，后端选择是场景设置而不是编译开关。

## 特性

- **2D 与 3D 场景** — 统一的 GameObject/Component 模型，层级、预制体式复制、序列化为可读的 `.cscene` JSON
- **双渲染后端，一致画面** — Vulkan GPU 后端（网格、精灵、文本、调试线、天空、级联阴影贴图、bloom → 调色 → 暗角 → FXAA，可选 4×/8× MSAA）与软件光栅化器，共用同一个场景收集前端
- **着色** — GGX 高光 + metallic/roughness、法线贴图、自发光、逐级联阴影偏移、天空渐变、雾、tonemap（none/Reinhard/ACES/filmic）
- **C++ 脚本** — 借鉴 Unity 的组件模型：`OnStart` / `OnUpdate` / `FixedUpdate` / 触发器与碰撞回调，可在编辑器或 CLI 里编译为共享库并热重载
- **编辑器** — 深色、致密、键盘优先的界面：层级、反射驱动的检视器、视口（选择/移动/旋转/缩放、Play/Pause/Step）、控制台、内容浏览器
- **代码编辑器** — Monaco，C++ 语法高亮、引擎 API 自动补全、编译诊断直接标注到行
- **图像编辑器** — 图层、笔刷/形状/填充、HSV 取色器、法线贴图生成；与 CLI 共用同一套无头栅格核心
- **命令行（CLI）** — `cairn` 以结构化 JSON 暴露引擎全部能力：场景/实体/组件/属性、无头 Play 模拟、脚本编译、截图、图像编辑；支持批量模式，专为 AI Agent 调用设计——**无 MCP 服务、无后台守护进程**
- **中英双语** — 编辑器界面完整中文化，语言即时切换

## 快速开始

要求：Node.js ≥ 20、CMake ≥ 3.16、C++20 编译器（g++ ≥ 11 / clang++ / MSVC）。Vulkan 头文件与 `glslc` 是可选的：缺少它们时引擎照常构建，只是走软件渲染。

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install          # Electron / esbuild / Monaco / node-addon-api
npm run build        # CMake 引擎库 + N-API 插件 + 渲染端打包
npm test             # 引擎单测 + 冒烟测试 + CLI 测试
npm start            # 启动编辑器
```

在启动画面新建项目（3D 沙盒或 2D 横版模板），按 **Ctrl+P** 进入 Play 模式。界面说明见 [docs/editor-manual.md](docs/editor-manual.md)，各平台构建与排错见 [docs/building.md](docs/building.md)。

## 如何使用引擎

三个入口，同一个引擎。按“谁在驱动”来选：

| 入口 | 什么时候用 | 从哪开始 |
| --- | --- | --- |
| **编辑器** | 人在搭游戏 | `npm start` → [编辑器手册](docs/editor-manual.md) |
| **C++ 脚本** | 写玩法逻辑 | `Assets/Scripts/*.cpp` → [脚本 API](docs/scripting-api.md) |
| **`cairn` CLI** | Agent、CI 脚本或自动化 | `node cli/index.js` → [CLI 参考](docs/cli.md) |

最小玩法脚本：

```cpp
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnUpdate(float deltaTime) override {
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += PropFloat("speed") * deltaTime;   // 检视器里可编辑的字段
        transform().SetEulerDegrees(euler);
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 90.0f);
    }
};

CAIRN_SCRIPT(Rotator)
```

在编辑器里按 **Ctrl+B** 编译，或用 `cairn script.build` 从命令行编译。

### 渲染后端

后端是场景设置，不是编译开关：

```bash
cairn --project ./game scene.settings.set --key renderBackend --value vulkan   # 或 auto / software
cairn --project ./game scene.settings.set --key msaa --value 4                # 1 / 2 / 4 / 8
```

`auto`（默认）在检测到 Vulkan 1.3 设备时走 GPU，不可用则静默回退软件。环境变量 `CAIRN_RENDER_BACKEND=auto|vulkan|software` 在进程级覆盖这个默认值，但**只对仍为 `auto` 的场景生效**——项目显式指定了后端就仍然尊重它。

## 让 Agent 用 CLI 做游戏

`cairn` 是一个纯命令行工具：每次调用在 stdout 输出恰好一个 JSON 文档，错误进 stderr，退出码 0/1/2。不用起守护进程、不用维护会话、没有需要保活的东西。

```bash
node cli/index.js engine.schema        # 全部命令与参数 schema——从这里开始
node cli/index.js component.schema     # 每个组件的属性名、类型与默认值
```

完整契约见 [docs/cli.md](docs/cli.md)。速查：

```bash
cairn --project ./game --scene Assets/Scenes/Main.cscene scene.tree
cairn --project ./game scene.settings.set --key renderBackend --value vulkan
cairn --project ./game script.build                  # 编译 Assets/Scripts/*.cpp
cairn --project ./game exec.run --file steps.jsonl   # 多步批量，共用一个会话
```

**用批量模式干活。** 每个进程都是全新会话，所以需要跨步骤保留的状态必须经由 `scene.save` / `image.save` 落盘——或者干脆把整串步骤放进一次 `exec.run`，让同一个引擎实例一直活着：

```jsonl
{"command":"entity.createPrimitive","args":{"mesh":"Cube","name":"Crate","position":[0,2,0]},"id":"crate"}
{"command":"component.add","args":{"id":"$ref:crate","type":"RigidBody","props":{"mass":2}}}
{"command":"component.setProps","args":{"id":"$ref:crate","type":"MeshRenderer","props":{"albedoColor":[0.9,0.4,0.2,1]}}}
{"command":"scene.save","args":{"file":"Assets/Scenes/Level1.cscene"}}
```

`"id"` 会把该步骤的结果存下来，后续步骤用 `"$ref":"<id>"` 引用。引用写错会直接报错，并把当前可用的 id 列出来。

### 一条完整流程

无头搭一个小关卡、挂脚本、跑模拟、然后看结果：

```bash
cairn --project ./game project.create --name OrbRun      # 3D 沙盒或 2D 横版模板
#   ... 写下 Assets/Scripts/Collector.cpp ...
cairn --project ./game script.build                      # 编译玩法 C++
cairn --project ./game exec.run --file level.jsonl       # 搭关卡（不要传 --scene：第一步就是 scene.new）
cairn --project ./game --scene Assets/Scenes/Level1.cscene exec.run --file play.jsonl
cairn --project ./game project.exportPlayer --target ./player
```

脚本必须先存在并编译通过，挂在它的那一批才能跑，所以顺序就按上面这样。

`Assets/Scripts/Collector.cpp`——小球自转，被碰到就消失：

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

`level.jsonl`——地面、玩家、挂了脚本的可收集物，以及一盏灯：

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

`play.jsonl`——把关卡跑起来并留下证据。不需要任何 id：东西都已经在存好的场景里了。

```jsonl
{"command":"runtime.play"}
{"command":"runtime.simulate","args":{"seconds":1}}
{"command":"runtime.logs","args":{"level":"info"}}
{"command":"render.screenshot","args":{"out":"/tmp/level.png","width":640,"height":400}}
{"command":"runtime.stop"}
```

`runtime.play` 会给场景拍快照，`runtime.stop` 再恢复回去；`runtime.simulate` 不开窗口就能推进固定步长。截图是你自查的手段——在宣布关卡做完之前，把 PNG 读回来看过。

### 会让 Agent 白白浪费一轮的坑

- **全局参数要放在命令名之前**：`cairn --project ./game --format text scene.tree`，不是 `cairn scene.tree --format text`。
- **`--value` 是 JSON**：向量写 `'[0,-0.5,0]'`。`tonemap` 和 `renderBackend` 额外接受裸名字（`aces`、`vulkan`）。
- **图元名首字母大写**：`Cube`、`Sphere`、`Plane`、`Quad`、`Cylinder`、`Cone`、`Torus`。
- **`component.setProps` 要求组件已存在**，先用 `component.add` 添加。
- **批量作业请固定后端**：GPU 的每一帧都会阻塞等回读 fence，几百帧下来会和桌面抢显卡；`CAIRN_RENDER_BACKEND=software` 让批量跑在 CPU 上。
- **先量再猜**：`cairn engine.profile` 给出各阶段耗时（`update`/`scripts`/`physics` 与 `render`/`gather`/`raster`/`post`/`gpu`），按耗时降序排列，并附带绘制计数与工作池遥测。`CAIRN_JOBS_WORKERS=1` 会把 CPU 各阶段强制串行，便于量出并行加速比。
- **看退出码，别只看 stdout**：失败的命令同样会打印 JSON，只是 `"ok": false` 且带一个 `code`，比如 `invalid_arg`、`prop_rejected`。

## 文档

| 文档 | 内容 |
| --- | --- |
| [架构总览](docs/architecture.md) | 模块划分、渲染后端、数据流、热重载机制 |
| [路线图与进度](docs/roadmap.md) | 阶段划分、已交付增量、下一步、已知限制 |
| [编辑器手册](docs/editor-manual.md) | 界面、键盘导航、工作流 |
| [CLI 参考](docs/cli.md) | 全部 `cairn` 命令、JSON 契约、批量模式、实用配方 |
| [脚本 API](docs/scripting-api.md) | 完整的 C++ 脚本接口 |
| [构建指南](docs/building.md) | 依赖、GPU 后端编译选项、各平台说明、排错 |

## 项目结构

```
engine/         C++ 运行时 —— math、json、scene、renderer、physics、script host
engine/shaders/ Vulkan 后端的 GLSL（构建时编译为 SPIR-V）
addon/          Engine 门面的 N-API 绑定（编辑器与 CLI 共用）
editor/main/    Electron 主进程 —— 窗口、对话框、项目读写、脚本编译
editor/renderer/ 编辑器 UI（原生 DOM + ES 模块，用 esbuild 打包）
runtime/        导出的 Player —— 加载项目并在无编辑器 UI 下运行
cli/            `cairn` 命令行
templates/      新建项目模板（3D 沙盒、2D 横版）
test/           冒烟测试 + CLI 测试
docs/           文档
```

## 状态

Cairn 当前版本 **0.1.17**——是个能用但仍早的起点。GPU 后端是默认路径但还年轻；软件光栅化器是参考实现，测试套件也固定跑它，因此**两条路径之间任何渲染差异都算 GPU 后端的 bug，而不是有意为之**。里程碑历史见 [CHANGELOG](CHANGELOG.md)，提 PR 前请先读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)
