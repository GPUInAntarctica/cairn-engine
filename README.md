<div align="center">

# Cairn Engine

**A compact C++ game engine with a focused editor for 2D and 3D projects.**

[![CI](https://github.com/cairn-engine/cairn-engine/actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Engine](https://img.shields.io/badge/engine-C%2B%2B20-00599C.svg)](docs/architecture.md)
[![Editor](https://img.shields.io/badge/editor-Electron-47848F.svg)](docs/editor-manual.md)

简体中文 · [English](#english)

</div>

Cairn 是一个精巧的游戏引擎：C++ 运行时负责渲染、物理与玩法脚本，Electron 编辑器负责场景搭建与内容创作。它没有庞大的依赖树——引擎核心零第三方库（仅 vendored 的 stb 与 Node 头文件），编辑器只依赖 Electron、Monaco 与 esbuild。

## 特性

- **2D 与 3D 场景** — 统一的 GameObject/Component 模型，层级、预制体式复制、序列化为可读的 `.cscene` JSON
- **软件渲染器** — 多线程光栅化（Blinn-Phong、天空渐变、雾、2D 精灵、位图字体），零 GPU 依赖，任何机器可跑
- **C++ 脚本** — 借鉴 Unity 的组件模型：`OnStart` / `OnUpdate` / `FixedUpdate` / 触发器回调，编辑器内一键编译为共享库并热重载
- **编辑器** — Unreal 风格深色界面：层级、检视器（反射驱动）、视口（选择/移动/旋转/缩放、Play/Pause/Step）、控制台、内容浏览器
- **代码编辑器** — Monaco，C++ 高亮、引擎 API 自动补全、编译诊断直接标注到行
- **图像编辑器** — 图层、笔刷/形状/填充、HSV 取色器、法线贴图生成（2D 光照可用）
- **中英双语** — 编辑器界面完整中文化，语言即时切换

## 快速开始

要求：Node.js ≥ 20、CMake ≥ 3.16、C++20 编译器（g++ ≥ 11 / clang++ / MSVC）、Electron 可自动下载。

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install          # Electron / esbuild / Monaco / node-addon-api
npm run build        # CMake 引擎库 + N-API 插件 + 渲染端打包
npm test             # 引擎单测 + 端到端冒烟（含脚本编译与热重载）
npm start            # 启动编辑器
```

在启动画面新建项目（3D 沙盒或 2D 横版模板），按 **Ctrl+P** 进入 Play 模式。

## 脚本示例

```cpp
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnUpdate(float deltaTime) override {
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += PropFloat("speed") * deltaTime;   // 检视器可编辑字段
        transform().SetEulerDegrees(euler);
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 90.0f);
    }
};

CAIRN_SCRIPT(Rotator)
```

完整 API 见 [docs/scripting-api.md](docs/scripting-api.md)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [架构总览](docs/architecture.md) | 模块划分、数据流、热重载原理 |
| [编辑器手册](docs/editor-manual.md) | 界面、快捷键、工作流 |
| [脚本 API](docs/scripting-api.md) | C++ 脚本接口完整参考 |
| [构建指南](docs/building.md) | 各平台构建、故障排查 |

## 路线图

- [ ] GPU 后端（OpenGL）作为软件渲染的替代路径
- [ ] 3D 模型导入扩展（glTF）
- [ ] 音频系统（当前为 WAV 事件占位）
- [ ] 光照贴图与阴影

欢迎通过 [CONTRIBUTING.md](CONTRIBUTING.md) 参与贡献。

## 许可证

[MIT](LICENSE)

---

<div align="center">

# English

</div>

**Cairn** is a compact game engine: a C++ runtime for rendering, physics and gameplay scripts, driven by an Electron editor for scene building and content creation. It keeps the dependency tree deliberately small — the engine core has no third-party libraries beyond vendored stb headers and Node's own headers, and the editor relies only on Electron, Monaco and esbuild.

### Features

- **2D and 3D scenes** — a unified GameObject/Component model with hierarchy, duplication and readable `.cscene` JSON serialization
- **Software renderer** — multithreaded rasterization (Blinn-Phong, sky gradient, fog, 2D sprites, bitmap font) with zero GPU requirements
- **C++ scripting** — Unity-style components: `OnStart` / `OnUpdate` / `FixedUpdate` / trigger callbacks, compiled to a shared library from the editor and hot-reloaded
- **The editor** — a dark, dense, Unreal-flavored workspace: hierarchy, reflection-driven inspector, viewport with select/move/rotate/scale and Play/Pause/Step, console, content browser
- **Code editor** — Monaco with C++ highlighting, engine API completion and build diagnostics mapped onto lines
- **Image editor** — layers, brush/shape/fill tools, HSV picker, and normal-map generation usable by the 2D lighting path
- **Bilingual UI** — full Simplified Chinese and English with instant switching

### Quick start

Requires Node.js ≥ 20, CMake ≥ 3.16 and a C++20 compiler (g++ ≥ 11 / clang++ / MSVC).

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install
npm run build     # engine library + native addon + renderer bundle
npm test          # engine unit tests + end-to-end smoke (script build & hot reload)
npm start         # launch the editor
```

Create a project from the startup screen (3D sandbox or 2D platformer template) and press **Ctrl+P** to play.

### Status

Cairn is at version **0.1.0** — an early, usable foundation. See the [roadmap](#路线图) and [CHANGELOG](CHANGELOG.md). Feedback and pull requests are welcome — please read [CONTRIBUTING.md](CONTRIBUTING.md) first.

Licensed under the [MIT License](LICENSE).
