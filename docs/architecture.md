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
| `engine/src/physics.cpp` | 半隐式欧拉积分，AABB/球/胶囊碰撞（3D）、XY 平面盒（2D）与三角面汤网格（`MeshCollider`），层矩阵、射线/重叠查询、角色控制器、距离/弹簧关节、休眠、碰撞与触发回调，以及可选的连续碰撞检测 |
| `engine/src/script_host.cpp` | `dlopen` 玩法库、注册表查询、热重载（复制到唯一文件名避免加载器缓存） |
| `engine/src/engine.cpp` | 门面：场景/资产/输入/播放状态机/拾取/投影，编辑器与 Player 共用 |
| `engine/src/prefab.cpp` | 预制体：`.prefab` 读写、实例化、apply/revert，以及逐属性覆盖（实例与资产子树的 diff、按记录回填资产的 sync） |
| `addon/cairn_addon.cc` | N-API 绑定，暴露引擎门面并共享帧缓冲 |
| `editor/main/` | Electron 主进程：窗口、原生对话框、项目系统、脚本编译管线、Player 导出、偏好 |
| `editor/renderer/` | 编辑器 UI（esbuild 打包为 CJS 以使用 Node 集成） |
| `editor/main/imaging.js` | 与 DOM 无关的栅格核心：分层文档、合成、笔刷/形状/填充、多边形与外侧描边、镜像反射、翻转/旋转/裁剪、贴图拼合、种子噪声与线性/径向渐变、调色板量化与抖动、文本预览与像素统计、Sobel 法线贴图、PNG 编解码、BMP 解码（图像编辑器与 CLI 共用；预览/形状/变换/纹理原语由 CLI 驱动） |
| `cli/` | `cairn` 命令行：命令注册表 + 参数 schema、JSON 输出契约、会话状态、批量模式（见 [cli.md](cli.md)） |
| `runtime/` | 导出的 Player：无编辑器 UI 的 Electron 窗口，加载项目并运行首场景 |

## 帧循环

编辑器进程的每一帧：

1. **输入** — 视口把键盘/鼠标事件注入 `Engine::input()`（编辑器相机飞行与脚本 `Input::` 共用一套状态）
2. **模拟** — Play 模式下 `Engine::Update(dt)` 累积固定步长（1/60）：先跑脚本 `FixedUpdate`，再走物理，随后可变步长 `OnUpdate`
3. **渲染** — 编辑器相机或场景主相机 → `GatherScene()` 产出 `FrameDrawList` → `FitCascadeSet()` 拟合级联 → 分派到后端。软件后端按三角形原子分发给工作线程；Vulkan 后端录制一条命令缓冲（阴影级联 → 主 pass → 后处理链），`vkQueueSubmit` 后阻塞等 fence
4. **呈现** — 两条路径都把结果写进同一块 `FrameTarget`，帧缓冲通过插件共享给 JS（零拷贝），`putImageData` 到 canvas；引擎日志与音频事件随后被轮询

## 物理

一步 `PhysicsStep(scene, dt)` 的顺序：**积分**（半隐式欧拉，重力/力/阻力 → 位置与角速度）→ **收集形状**（积分之后按世界矩阵收集，形状数据因此不会与物体脱节）→ **broadphase**（X 轴 sweep-and-prune，候选对再排回下标序以保证回调顺序稳定）→ **窄相**（`Collide()` 求接触与触发重叠，实体对走冲量 + 恢复系数 + 摩擦，位置修正按逆质量分配）→ **关节求解** → **休眠**。3D 与 2D 形状永不混对。

**连续碰撞检测（CCD）是逐刚体可选的。** 默认（`collisionDetection = Discrete`）完全走上面的离散流程：先把物体按 `velocity * dt` 移过去，再测重叠。因此单步位移大于障碍厚度时（子弹、长距离下落）会直接穿过去。选了 `Continuous` 的刚体在积分前多一趟**保守推进（conservative advancement）**：从当前位置出发，沿运动方向只推进「到最近正在接近的障碍的距离」，所以构造上**不可能越过接触点**；反复迭代逼近真实碰撞时刻，几何上受限的迭代次数只会影响掠过式逼近的精度，不会让它穿过去。位移被截断到碰撞时刻之后一点点（与物体尺度成比例的 skin），于是剩下的响应完全交给原有的窄相——冲量、恢复系数、摩擦、`OnCollisionEnter/Stay/Exit` 都不需要第二套代码。用保守推进而不是固定采样，是为了让**薄墙**（比一步位移薄得多）也能挡住，而且斜向命中停在真实表面上，而不是被放大的包围盒提前截停。

距离函数按窄相建模的每一对形状各写一份（球/胶囊精确，盒子与胶囊-盒用窄相同款的 AABB 间隙），所以 CCD 与窄相对「接触」的定义一致。扫描目标只有实体静态/运动学碰撞体（对应 Unity 的 `CollisionDetectionMode.Continuous`，不是 `ContinuousDynamic`），并且**步首就已经在接触的障碍会被排除**——否则一个安静躺在地板上的物体会读到零碰撞时刻，连滑动都会被冻住。没有刚体开启 CCD 时，这一整块（收集 + 扫描 + 新代码路径）都不会执行，场景行为与之前逐位相同。

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

**GPU 侧把相邻的同款绘制合并成一次实例化调用。** 不透明网格的逐实例数据（模型矩阵 + 材质 + 贴图槽位）放在一块 64 KiB 的 uniform 窗口里，步长固定 256 字节——这个数字必须同时满足 `minUniformBufferOffsetAlignment`（规范上限 256）和着色器里 `draws[]` 的 std140 数组步长，所以 CPU 端的 `DrawUbo` 补齐到 256 字节，着色器端用 `vec4 texIndex; vec4 pad[5]` 补齐（std140 里 `float arr[N]` 的步长是 16 字节，直接写 `float pad[24]` 会得到 384 字节，这是踩过的坑）。绘制时只把**相邻**且网格、剔除模式相同的绘制并入一个 run，动态偏移指向 run 的首元素，顶点着色器读 `draws[gl_InstanceIndex]` 并用 `flat` varying 把下标传给片元着色器；run 不跨 64 KiB 窗口，超过就断开。只合并相邻 run 保证了不同网格之间的绘制顺序不变，因此画面与逐绘制路径**逐位相同**（用 `CAIRN_NO_INSTANCING=1` 可以关掉合并做 A/B 对比）。`RenderStats::drawCalls` 在 GPU 路径上按 API 提交计数，所以一次实例化 run 只算一次。

**贴图走 bindless 数组，所以合批不再要求贴图相同。** 场景管线布局的 set 1 是两个 `sampler2D` 数组（albedo、normal 各 512 槽），取代了原来的"每次绘制一组 albedo/normal 描述符"。一次绘制把两个槽位下标写进绘制 uniform（`DrawUbo.texIndex`），片元着色器用 `nonuniformEXT` 索引数组取样，于是同一个实例化 run 里每个实例都能用**自己的**贴图——60 个方块、6 张交错贴图的场景从 63 次绘制降到 4 次（1 次不透明 run + 2 次阴影 run + 天空），且画面与合并前逐位相同。槽位表按 `(贴图, 采样器)` 建立、跨帧稳定，所以热场景一帧里一次描述符写入都不需要；槽 0 固定是 1×1 白色贴图，"没有贴图"就指向它，因此数组里每个槽位始终是合法描述符。贴图缓存淘汰某张贴图时会把它的槽位写回白色并归还空闲表；若所有槽位都在用，则抢占最近最少使用的一个——最坏情况是槽位抖动，而不会把某张贴图静默降级成白色。采样器是槽位键的一部分，所以精灵的 `pixelPerfect`（clamp）和网格的（repeat）可以共存；这顺带修掉一个潜在 bug：旧的 `textureView` 助手会把调用方的采样器覆盖成 `linearRepeat`，导致 `pixelPerfect` 精灵与位图文字按 repeat 取样，现在它们按 clamp 取样，与软件参考一致。

**描述符索引是硬性设备要求。** 后端在创建设备时查询并启用 `shaderSampledImageArrayDynamicIndexing`、`descriptorIndexing`、`shaderSampledImageArrayNonUniformIndexing`，并要求 `maxPerStageDescriptorSampledImages >= 1024`。不满足的设备会带着日志原因让后端初始化失败，由软件光栅化接管，而不是悄悄丢掉这个特性。


**阴影通道同样实例化，而且级联矩阵只推一次。** 每个 caster 的模型矩阵也来自同一块 `draws[]`，级联的 light-view-proj 是该通道内所有 caster 共用的，所以每个级联只 `CmdPushConstants` 一次，再把相邻且网格/剔除模式相同的可见 caster 合成一个 run。窗口寻址用和主通道一样的 `window/offset` 算法——旧代码固定绑窗口 0，一旦一帧的绘制数超过 `drawUboChunk`（256），caster 的动态偏移就会越过 64 KiB 描述符范围（实测 300 个物体的场景会落到窗口 1、2，修复后校验层干净）。

**单级联阴影曾经会崩溃。** 阴影贴图用 `layers = 级联数` 创建，而 `CreateImage` 只在 `layers > 1` 时建立逐层视图；`shadows = 1` 时阴影通道却仍然去取 `layerViews[0]`，越界读导致 SIGSEGV（在 GPU 后端、且只在这个级联数下复现，软件路径不受影响）。现在单层贴图直接复用整图视图。

## 任务系统与性能分析

**一个进程级的工作池支撑所有并行。** `Cairn::Jobs::ParallelFor(count, f)` 把下标 `0..count-1` 动态分发给工作线程，调用线程自己也参与，因此负载天然均衡。池在首次使用时惰性创建（`hardware_concurrency` 上限 16），进程内只此一份；`CAIRN_JOBS_WORKERS=1` 会让它退化为串行执行，方便对照测量。

**并行只用在"下标无关"的地方。** 一个 `f(i)` 不得依赖其它下标的结果，也不得与其它下标写同一块内存。需要确定性结果的调用方应各自写入按下标分配的槽位，再按下标顺序合并——软件光栅化就是这样按扫描行分带（每条带只写自己的行），后处理（泛光、调色、暗角、FXAA）与天空/清除也按行并行。`f` 内部**禁止再次调用** `ParallelFor`：嵌套分发会覆盖正在执行的任务，因此嵌套调用被降级为串行。

**确定性优先于吞吐。** 并行不改变任何一个像素的取值——每条行只读共享输入、只写自己的像素，所以软件路径的输出与串行版本逐位相同，测试套件里固定的像素断言因此仍然成立。真正的收益在重负载上：1280×720、300 个物体、开启全部后处理时，后处理链从 57.6 ms 降到 6.4 ms（约 8.9×），整帧从约 70 ms 降到约 20 ms。

**帧剖析器。** `Profiler`（`<cairn/profiler.h>`）用 `BeginSample`/`EndSample` 或 RAII 的 `CAIRN_PROFILE("名字")` 记录命名样本，按名字维护约 1 秒的滚动窗口（last/avg/max）。引擎在固定位置插桩：`update`、`scripts`、`physics` 来自 `Engine::Update`，`render`、`gather`、`shadows`、`raster`、`post`、`gpu` 来自共享的 `Renderer::RenderScene`。每帧提交一次，`engine.profile` 命令读出快照（按耗时降序排列）并附带本帧的绘制计数与任务系统遥测。
