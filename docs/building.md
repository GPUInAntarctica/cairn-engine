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
npm test             # 引擎单测 + 端到端冒烟
npm start            # 启动编辑器
```

分步构建：

```bash
npm run build:engine # CMake → build/libcairn_engine.so
npm run build:addon  # 插件 → build/cairn_engine_addon.node
node scripts/build.js renderer  # 渲染端 → editor/renderer/dist/
```

## GPU 渲染 / GPU rendering

Vulkan 后端默认开启（`CAIRN_ENABLE_VULKAN=ON`），需要 Vulkan 头文件和 `glslc`。两者任一缺失时构建照常通过，只是 `VulkanBackendAvailable()` 返回 false，渲染固定走软件光栅化。

```bash
npm run build -- -DCAIRN_ENABLE_VULKAN=OFF   # 强制软件后端（无着色器编译）
```

运行时不需要链接 Vulkan loader：`libvulkan.so.1` 通过 `dlopen` 载入，设备级命令用 `vkGetDeviceProcAddr` 解析。后端要求 Vulkan 1.3（dynamic rendering + synchronization2 为硬性依赖），并且场景设置或环境变量 `CAIRN_RENDER_BACKEND=vulkan` 强制 GPU。

GPU 帧以 `vkCmdCopyImageToBuffer` 阻塞回读结束，宿主拿到的仍是普通像素缓冲。批量渲染时用 `CAIRN_RENDER_BACKEND=software` 可以避免回读与显卡争用。

## 平台说明 / Platforms

### Linux
开箱即用。若 `node-gyp` 类报错，确认发行版带有 Node 头文件（`/usr/include/node`）。

### macOS
引擎与插件使用 clang++，插件以 `-undefined dynamic_lookup` 链接（Node 符号运行时解析）。渲染与物理为纯 CPU 实现，无 Metal 依赖。

### Windows
- 引擎库：CMake + MSVC（或 MinGW g++）
- 插件：需要 MSVC 链接 `node.lib`（见 `docs/` 后续补充的脚本）；当前构建脚本对 win32 给出指引
- 模板与编辑器逻辑均跨平台

## 故障排查 / Troubleshooting

**`npm test` 冒烟失败：找不到编译器**
偏好设置中指定编译器路径，或确认 `g++ --version` 可用。

**启动时 `Cannot find module .../build/cairn_engine_addon.node`**
先运行 `npm run build`。

**编辑器空白 / 渲染进程报错**
`npm run build` 重新生成 `editor/renderer/dist/`；确认 `node_modules/monaco-editor` 存在。

**脚本编译报 `undefined symbol`**
确认项目 `Library/` 下引擎库路径与安装位置一致（开发仓库内直接使用无需处理）。

**Windows 下脚本热重载失败**
NTFS 文件锁：确保停止 Play 模式后再构建。
