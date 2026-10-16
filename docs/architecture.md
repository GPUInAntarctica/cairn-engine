# 架构 / Architecture

渲染管线要点：两条后端共用同一个场景收集前端，每帧从 view-projection 提取**视锥**做包围盒剔除，网格**包围盒缓存**，纹理带 **mipmap 链 + 三线性**过滤。软件光栅化按**水平条带**并行（每个线程独占一段行，帧缓冲与深度缓冲因此无数据竞争）；GPU 后端走 Vulkan dynamic rendering，最后把颜色与深度拷回宿主可 blit 的缓冲。

Cairn 由四个主要部分组成：**C++ 引擎运行时**、**N-API 插件**、**Electron 编辑器**和 **`cairn` 命令行**（编辑器与 CLI 共用同一个 N-API 门面；CLI 不是 MCP）。GPU 后端在运行时通过 `dlopen` 载入 Vulkan，不需要链接期依赖；没有可用设备时整条路径退回软件光栅化，因此引擎在无显卡环境依然可用。

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

## 渲染后端

两条光栅化路径共用同一个前端：`GatherScene()` 产出 `FrameDrawList`（不透明网格、2D 项、阴影投射体、调试线、灯光、级联矩阵），`FitCascadeSet()` 拟合阴影视锥，之后才分派到某个后端。

- **Vulkan（默认）。** `RenderBackend::Auto` 在运行时探测到可用的 Vulkan 1.3 设备时走 GPU，否则静默回退软件；`RenderBackend::Vulkan` 强制 GPU，失败会记日志并回退。GPU 帧最后用 `vkCmdCopyImageToBuffer` 阻塞回读，因此宿主拿到的始终是一份可 blit 的像素。
- **软件。** `RenderBackend::Software` 强制 CPU 光栅化，MSAA 与逐像素阴影统计（`RenderStats::shadowedPixels`）只在这条路径上有定义；像素级断言的测试都固定走这条路径。

两条路径共用 `FrameDrawList` 里的几何、材质、灯光与级联矩阵，因此剔除、排序与阴影视锥拟合不会在两边漂移。差异集中在光栅化本身：MSAA 与逐像素阴影统计（GPU 上阴影由硬件解析，统计量恒为 0），以及 y 轴翻转——Vulkan 的裁剪空间 y 向下，所有顶点阶段翻转 y、面朝向随之取顺时针，阴影查找也必须用同一套坐标（`shadow.vert` 翻转后，采样不能再翻一次）。

场景设置 `renderBackend`（auto/vulkan/software）与 `msaa` 决定选择；环境变量 `CAIRN_RENDER_BACKEND` 可在进程级覆盖（测试套件用它把渲染固定在 CPU 上，避免数百次阻塞回读抢占显卡）。

## 模块

| 模块 | 职责 |
| --- | --- |
| `engine/include/cairn/math.h` | 纯头文件线性代数（Vec2/3/4、Quat、Mat4、AABB、Color），右手系、Y 向上、列主序 |
| `engine/src/json.*` | 保序 JSON 解析/序列化，用于场景、项目与编辑器通信 |
| `engine/src/asset.*` | 资产数据库：`Assets/` 下每个文件一个 `.meta` 边车（稳定 GUID、importer、导入设置），维护 GUID↔路径索引；场景引用按 GUID 解析，重命名/移动资产不断链 |
| `engine/src/scene.cpp` | GameObject/Component 容器、Transform 父子链、延迟销毁、`.cscene` 读写 |
| `engine/src/components.cpp` | 反射表：每个引擎组件暴露 属性名/类型/get/set，检视器与序列化共用 |
| `engine/src/renderer.cpp` | 渲染前端 + 多线程软件光栅化：场景收集（`GatherScene`）、阴影视锥拟合（`FitCascadeSet`）、后端分发、裁剪（六面体）、透视校正插值、GGX、天空、雾、2D 精灵/文本、调试线 |
| `engine/src/backend_vulkan.cpp` | Vulkan 1.3 GPU 后端（dynamic rendering + synchronization2，运行时 `dlopen`）：网格/精灵/线/天空管线、MSAA 解析、级联阴影数组、纹理与 mipmap 上传、后处理链、颜色与深度回读。`engine/src/vk_loader.h` 负责符号解析（设备级命令必须走 `vkGetDeviceProcAddr`） |
| `engine/shaders/*.vert,frag` | GLSL 源，构建时由 `glslc` 编译为 SPIR-V 并嵌入库（`engine/cmake/embed_spirv.cmake`）；缺少 Vulkan 头文件或编译器时整个后端被裁掉 |
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
3. **渲染** — 编辑器相机或场景主相机 → `GatherScene()` 产出 `FrameDrawList` → `FitCascadeSet()` 拟合级联 → 分派到后端。软件后端按三角形原子分发给工作线程；Vulkan 后端录制一条命令缓冲（阴影级联 → 主 pass → 后处理链），`vkQueueSubmit` 后阻塞等 fence
4. **呈现** — 两条路径都把结果写进同一块 `FrameTarget`，帧缓冲通过插件共享给 JS（零拷贝），`putImageData` 到 canvas；引擎日志与音频事件随后被轮询

## 脚本热重载

1. 主进程将 `Assets/Scripts/**/*.cpp` 编译为 `Library/GameScripts.so`（链接 `libcairn_engine`，`-fno-gnu-unique` 至关重要——否则 GCC 的 unique 符号会跨 `dlclose` 存活，旧库的注册表会污染新库）
2. `ScriptHost::Load` 把编译产物**复制到唯一文件名**再 `dlopen`（绕过加载器按路径缓存），查询导出符号 `CairnQueryScriptRegistry`
3. 新库加载前，引擎销毁所有脚本实例并清空缓存的类元数据（其中 Json 默认值持有指向旧库代码的删除器——顺序错了会踩进已卸载内存）
4. 检视器读取类元数据（`DescribeProps`）生成字段 UI，值存于 `ScriptComponent::properties`，重载后原样恢复

## 两条渲染路径的分工

**GPU 是默认路径。** Vulkan 1.3 + dynamic rendering，运行时 `dlopen`，设备级命令走 `vkGetDeviceProcAddr`（`vkGetInstanceProcAddr` 在部分驱动上对设备命令只返回桩函数，这条不能走）。没有链接期依赖意味着同一个二进制在一台没有 Vulkan 的机器上照样能跑。

**软件路径是参考实现。** 它定义了这个引擎"正确"的画面长什么样：GPU 后端存在的意义是加速，不是引入第二套语义。测试套件固定跑软件路径，因此两条路径之间出现的任何渲染差异都算 GPU 侧的 bug。

**共同的取舍是阻塞回读。** GPU 帧最后用 `vkCmdCopyImageToBuffer` 把颜色与深度拷进宿主可见缓冲，再等 fence——因为宿主（Electron canvas、CLI 截图）要的就是一份普通像素。这一步让 GPU 路径天然不适合"渲染几千帧但一张都不要"的场景：批量作业应当用 `CAIRN_RENDER_BACKEND=software`。

后端选择的决策点在 `Renderer::RenderScene`：`Auto` 优先 GPU 并静默回退，`Vulkan` 强制 GPU（失败记日志后回退），`Software` 永远走 CPU。
