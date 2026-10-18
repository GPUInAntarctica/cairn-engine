# Building / 构建指南

## 依赖 / Requirements

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| Node.js | ≥ 20 | npm、插件构建（N-API 头文件随 Node 分发） |
| CMake | ≥ 3.16 | 引擎库构建 |
| C++ 编译器 | C++20 | g++ ≥ 11 / clang++ / MSVC 2019+ |
| Electron | 自动安装 | 编辑器与 Player |
| Vulkan 头文件 | 可选 | GPU 后端；缺失时自动关闭，引擎仍可软件渲染 |
| `glslc` | 可选 | 把 `engine/shaders/*.vert,frag` 编译为 SPIR-V 并嵌入库 |

## 构建 / Build

```bash
npm install
npm run build        # 全部：引擎库 + 插件 + 渲染端
npm run rebuild      # 强制重新配置 CMake
npm test             # 引擎单测 + 端到端冒烟 + CLI 测试
npm run test:engine  # 只跑 C++ 引擎单测
npm start            # 启动编辑器
```

分步构建：

```bash
npm run build:engine # CMake → build/libcairn_engine.so
npm run build:addon  # 插件 → build/cairn_engine_addon.node
node scripts/build.js renderer  # 渲染端 → editor/renderer/dist/
```

产物先落在 `engine/build/`，再由脚本暂存到仓库根的 `build/`（插件就链接这里的那份）。CMake 的
**多配置**生成器（Visual Studio、Xcode、Ninja Multi-Config）默认会给输出目录再加一层配置子目录
（`Release/`、`Debug/`…）并默认构建 Debug；`engine/CMakeLists.txt` 把输出目录连同逐配置变体一起
钉平，`scripts/build.js` 也会用 `--config Release` 明确配置、并仍然在逐配置子目录里兜底搜索。
所以手工配置出来的构建树（哪怕是旧版本配置的）也能被正确暂存——这套布局由 `node test/build.js` 钉住。

## GPU 渲染 / GPU rendering

Vulkan 后端默认开启（`CAIRN_ENABLE_VULKAN=ON`），需要 Vulkan 头文件和 `glslc`。两者任一缺失时构建照常通过，只是 `VulkanBackendAvailable()` 返回 false，渲染固定走软件光栅化。

```bash
npm run build -- -DCAIRN_ENABLE_VULKAN=OFF   # 强制软件后端（跳过着色器编译）
npm run build -- -DCAIRN_ENABLE_VULKAN=ON    # 改回来
```

`-D` 选项会被转发给 CMake 并触发重新配置。注意 CMake 会**缓存**这个值：一旦置为 `OFF`，之后普通的 `npm run build` 仍然是关的，必须显式再传 `-DCAIRN_ENABLE_VULKAN=ON`，或者删掉 `engine/build/` 从头配置。

运行时不需要链接 Vulkan loader：`libvulkan.so.1` 通过 `dlopen` 载入，设备级命令用 `vkGetDeviceProcAddr` 解析。后端要求 Vulkan 1.3（dynamic rendering + synchronization2 为硬性依赖），并且场景设置或环境变量 `CAIRN_RENDER_BACKEND=vulkan` 强制 GPU。

GPU 帧以 `vkCmdCopyImageToBuffer` 阻塞回读结束，宿主拿到的仍是普通像素缓冲。批量渲染时用 `CAIRN_RENDER_BACKEND=software` 可以避免回读与显卡争用。

## 平台说明 / Platforms

### Linux
开箱即用。若 `node-gyp` 类报错，确认发行版带有 Node 头文件（`/usr/include/node`）。

### macOS
引擎与插件使用 clang++，插件以 `-undefined dynamic_lookup` 链接（Node 符号运行时解析）。渲染走 Vulkan（MoltenVK）或软件光栅化，无 Metal 依赖。

### Windows

一键构建使用 MSVC。请从 **“Developer Command Prompt for VS”**（或先运行 `vcvars64.bat`）里执行，让 `cl.exe` 进入 `PATH`：

```bat
npm install
npm run build
```

构建流程：CMake 用 MSVC 生成 `cairn_engine.dll` + `cairn_engine.lib`（`WINDOWS_EXPORT_ALL_SYMBOLS` 导出全部符号）；插件用 `cl /LD` 编译、链接 `cairn_engine.lib` 与 Node 自带的 `node.lib`；玩法脚本（`editor/main/build.js`）同样走 `cl /LD`。需要 Node 安装包含头文件（`<node>\include\node`）与 `node.lib`；若缺失，脚本会明确提示。

若看到 `cl.exe not found. Open a "Developer Command Prompt for VS"...`，说明当前不是 VS 开发者环境；用上述命令提示符重开终端即可。避免在 Play 模式下重新编译脚本——Windows 会锁定已加载的 `GameScripts.dll`。

## 故障排查 / Troubleshooting

**`npm test` 冒烟失败：找不到编译器**
偏好设置中指定编译器路径，或确认 `g++ --version` 可用。`CXX` 环境变量可以覆盖编译器。

**`npm run test:engine` 提示找不到 `cairn_engine_test`**
先 `npm run build`——引擎单测由 CMake 一并构建。

**`npm run build` 报 `engine library output not found`**
CMake 那一步成功了，但脚本在预期的目录里没找到库——典型原因是多配置生成器把产物放进了
配置子目录（或构建树是旧版本配置的）。脚本会兜底搜索那些子目录；仍然报错就删掉 `engine/build/`
从头配置一次。`node test/build.js` 单独检查这套发现与暂存逻辑。

**`npm run rebuild` 好像什么都没做**
它会重新跑 CMake 配置并全量编译；耗时取决于 CPU 核数。若想确认确实重编了，删掉 `engine/build/` 再 `npm run build`。

**画面全黑，或日志里有 `Vulkan backend unavailable`**
GPU 后端不可用时会自动回退软件，功能不受影响。想确认当前走的是哪条路，看 `RenderStats::backend`（CLI 里 `render.editorFrame` 的返回值里有）。强制指定：`scene.settings.set --key renderBackend --value software`，或环境变量 `CAIRN_RENDER_BACKEND=software`。

**批量跑图时桌面卡顿**
GPU 每帧都以阻塞回读结束，几百帧会和合成器抢显卡。批量作业加 `CAIRN_RENDER_BACKEND=software`。

**启动时 `Cannot find module .../build/cairn_engine_addon.node`**
先运行 `npm run build`。

**编辑器空白 / 渲染进程报错**
`npm run build` 重新生成 `editor/renderer/dist/`；确认 `node_modules/monaco-editor` 存在。

**脚本编译报 `undefined symbol`**
确认项目 `Library/` 下引擎库路径与安装位置一致（开发仓库内直接使用无需处理）。

**Windows 下脚本热重载失败**
NTFS 文件锁：确保停止 Play 模式后再构建。
