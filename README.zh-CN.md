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

渲染采用**软件光栅化**：无需 GPU、无需驱动、不依赖任何系统图形 API，在任何机器上都能跑。

## 特性

- **2D 与 3D 场景** — 统一的 GameObject/Component 模型，层级、预制体式复制、序列化为可读的 `.cscene` JSON
- **软件渲染器** — 多线程光栅化，GGX 高光、级联阴影贴图、天空渐变、雾、2D 精灵、位图字体
- **后处理** — bloom → 调色 → 暗角 → FXAA，每一步都可独立开关，且在中性设置下严格直通
- **C++ 脚本** — 借鉴 Unity 的组件模型：`OnStart` / `OnUpdate` / `FixedUpdate` / 触发器与碰撞回调，编辑器内一键编译为共享库并热重载
- **编辑器** — 深色、致密、键盘优先的界面：层级、反射驱动的检视器、视口（选择/移动/旋转/缩放、Play/Pause/Step）、控制台、内容浏览器
- **代码编辑器** — Monaco，C++ 语法高亮、引擎 API 自动补全、编译诊断直接标注到行
- **图像编辑器** — 图层、笔刷/形状/填充、HSV 取色器、法线贴图生成；与 CLI 共用同一套无头栅格核心
- **命令行（CLI）** — `cairn` 以结构化 JSON 暴露引擎全部能力：场景/实体/组件/属性、无头 Play 模拟、脚本编译、截图、图像编辑；支持 stdin 批量模式，专为 AI Agent 调用设计——**无 MCP 服务、无后台守护进程**
- **中英双语** — 编辑器界面完整中文化，语言即时切换

## 快速开始

要求：Node.js ≥ 20、CMake ≥ 3.16、C++20 编译器（g++ ≥ 11 / clang++ / MSVC）。

```bash
git clone https://github.com/cairn-engine/cairn-engine.git
cd cairn-engine
npm install          # Electron / esbuild / Monaco / node-addon-api
npm run build        # CMake 引擎库 + N-API 插件 + 渲染端打包
npm test             # 冒烟测试 + CLI 测试
npm start            # 启动编辑器
```

在启动画面新建项目（3D 沙盒或 2D 横版模板），按 **Ctrl+P** 进入 Play 模式。

## 命令行

```bash
node cli/index.js engine.schema                  # 自描述：全部命令与参数
node cli/index.js --project ./game scene.tree    # 读取场景
node cli/index.js --project ./game script.build  # 编译并加载玩法脚本
node cli/index.js --project ./game exec.run --file steps.jsonl   # 批量执行
```

所有命令默认在 stdout 输出**恰好一个** JSON 文档
（`{"ok":true,"command":...,"result":...}`），错误输出到 stderr
（`{"ok":false,"error":{"code","message","details"}}`）并以非 0 退出码结束；
`--format text` 提供人类可读输出。完整说明见 [docs/cli.md](docs/cli.md)。

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

完整脚本接口见 [docs/scripting-api.md](docs/scripting-api.md)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [**技术手册**](docs/technical-manual.md) | **完整参考**：坐标系、渲染管线、PBR 着色、阴影映射、后处理链、线程模型、数据模型、反射系统、物理、脚本宿主、文件格式、构建系统 |
| [架构总览](docs/architecture.md) | 模块划分、数据流、热重载原理 |
| [编辑器手册](docs/editor-manual.md) | 界面、键盘导航、工作流 |
| [命令行手册](docs/cli.md) | `cairn` 命令、JSON 契约、批量模式 |
| [脚本 API](docs/scripting-api.md) | C++ 脚本接口完整参考 |
| [构建指南](docs/building.md) | 各平台构建、故障排查 |

## 项目结构

```
engine/         C++ 运行时 — math、json、scene、renderer、physics、script host
addon/          Engine 门面的 N-API 绑定（编辑器与 CLI 共用）
editor/main/    Electron 主进程 — 窗口、对话框、项目读写、脚本编译
editor/renderer/编辑器 UI（原生 DOM + ES 模块，esbuild 打包）
runtime/        导出的 Player — 加载项目并运行，不含编辑器 UI
cli/            `cairn` 命令行
templates/      新建项目模板（3D 沙盒、2D 横版）
test/           冒烟测试 + CLI 测试
docs/           文档
```

## 状态

Cairn 当前版本为 **0.1.0** —— 早期但可用的基础。里程碑历史见
[CHANGELOG](CHANGELOG.md)，提交 PR 前请先阅读
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)