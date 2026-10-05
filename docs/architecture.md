# 架构 / Architecture

渲染管线要点：软件光栅化按**水平条带**并行（每个线程独占一段行，帧缓冲与深度缓冲因此无数据竞争），每帧从 view-projection 提取**视锥**做包围盒剔除，网格**包围盒缓存**，纹理带 **mipmap 链 + 三线性**过滤。

Cairn 由四个主要部分组成：**C++ 引擎运行时**、**N-API 插件**、**Electron 编辑器**和 **`cairn` 命令行**（编辑器与 CLI 共用同一个 N-API 门面；CLI 不是 MCP）。渲染采用软件光栅化，因此整个引擎不依赖 GPU 或任何系统图形库。

```
┌────────────────────────────────────────────────────────────┐
│                   Electron 编辑器 (renderer)                │
│  视口 / 层级 / 检视器 / 控制台 / 内容浏览器 / Monaco / 图像编辑器  │
└───────────────▲────────────────────────────▲───────────────┘
                │ N-API（同进程，零拷贝帧缓冲）      │ IPC
┌───────────────┴───────────────┐    ┌───────┴───────────────┐
│   cairn_engine_addon.node     │    │  Electron 主进程        │
│   （Engine 门面的绑定层）          │    │  窗口/对话框/编译管线/偏好  │
└───────────────┬───────────────┘    └───────────▲───────────┘
                │ 链接                            │ spawn g++
┌───────────────┴───────────────────────────────┴───────────┐
│                  libcairn_engine.so                        │
│   math · json · scene · renderer · physics · script_host   │
│                     · assets · engine 门面                  │
└────────────────────────────▲───────────────────────────────┘
                             │ 链接（编辑器内编译）
┌────────────────────────────┴───────────────────────────────┐
│              GameScripts.so（Assets/Scripts/*.cpp）          │
│        class Rotator : public Component { ... }              │
└─────────────────────────────────────────────────────────────┘
```

## 模块

| 模块 | 职责 |
| --- | --- |
| `engine/include/cairn/math.h` | 纯头文件线性代数（Vec2/3/4、Quat、Mat4、AABB、Color），右手系、Y 向上、列主序 |
| `engine/src/json.*` | 保序 JSON 解析/序列化，用于场景、项目与编辑器通信 |
| `engine/src/scene.cpp` | GameObject/Component 容器、Transform 父子链、延迟销毁、`.cscene` 读写 |
| `engine/src/components.cpp` | 反射表：每个引擎组件暴露 属性名/类型/get/set，检视器与序列化共用 |
| `engine/src/renderer.cpp` | 多线程软件光栅化：裁剪（六面体）、透视校正插值、Blinn-Phong、天空、雾、2D 精灵/文本、调试线 |
| `engine/src/physics.cpp` | 半隐式欧拉积分，AABB/球碰撞（3D）与 XY 平面盒（2D），触发器事件回调脚本 |
| `engine/src/script_host.cpp` | `dlopen` 玩法库、注册表查询、热重载（复制到唯一文件名避免加载器缓存） |
| `engine/src/engine.cpp` | 门面：场景/资产/输入/播放状态机/拾取/投影，编辑器与 Player 共用 |
| `addon/cairn_addon.cc` | N-API 绑定，暴露引擎门面并共享帧缓冲 |
| `editor/main/` | Electron 主进程：窗口、原生对话框、项目系统、脚本编译管线、Player 导出、偏好 |
| `editor/renderer/` | 编辑器 UI（esbuild 打包为 CJS 以使用 Node 集成） |
| `editor/main/imaging.js` | 与 DOM 无关的栅格核心：分层文档、合成、笔刷/形状/填充、Sobel 法线贴图、PNG 编解码、BMP 解码（图像编辑器与 CLI 共用） |
| `cli/` | `cairn` 命令行：命令注册表 + 参数 schema、JSON 输出契约、会话状态、批量模式（见 [cli.md](cli.md)） |
| `runtime/` | 导出的 Player：无编辑器 UI 的 Electron 窗口，加载项目并运行首场景 |

## 帧循环

编辑器进程的每一帧：

1. **输入** — 视口把键盘/鼠标事件注入 `Engine::input()`（编辑器相机飞行与脚本 `Input::` 共用一套状态）
2. **模拟** — Play 模式下 `Engine::Update(dt)` 累积固定步长（1/60）：先跑脚本 `FixedUpdate`，再走物理，随后可变步长 `OnUpdate`
3. **渲染** — 编辑器相机或场景主相机 → 收集可见网格 → 工作线程分片光栅化（按三角形原子分发）→ 2D 精灵按层排序混合 → 网格线/选区
4. **呈现** — 帧缓冲通过插件拷贝给 JS，`putImageData` 到 canvas；引擎日志与音频事件随后被轮询

## 脚本热重载

1. 主进程将 `Assets/Scripts/**/*.cpp` 编译为 `Library/GameScripts.so`（链接 `libcairn_engine`，`-fno-gnu-unique` 至关重要——否则 GCC 的 unique 符号会跨 `dlclose` 存活，旧库的注册表会污染新库）
2. `ScriptHost::Load` 把编译产物**复制到唯一文件名**再 `dlopen`（绕过加载器按路径缓存），查询导出符号 `CairnQueryScriptRegistry`
3. 新库加载前，引擎销毁所有脚本实例并清空缓存的类元数据（其中 Json 默认值持有指向旧库代码的删除器——顺序错了会踩进已卸载内存）
4. 检视器读取类元数据（`DescribeProps`）生成字段 UI，值存于 `ScriptComponent::properties`，重载后原样恢复

## 为什么软件渲染

编辑器要能在任何环境即刻运行——无 GPU、无驱动、无 XGL 依赖。渲染器已按块并行化，在普通桌面 CPU 上足以流畅驱动编辑器规模（数千三角形）的场景。未来会加入可选的 OpenGL 后端（见路线图），软件路径保留为兜底。
