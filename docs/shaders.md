# 材质着色器 / Material Shaders

Cairn 的材质着色器是**逐片元的标量表达式程序**：一个 `.cshader` 文件（或内置预设）描述三个输出——`du`、`dv`（UV 偏移）与 `emissive`（自发光掩码）。同一条字节码由两条渲染路径执行：软件光栅化器在 C++ 里解释，Vulkan 片元阶段在 GLSL 里解释。因此带自定义着色器的画面仍然有 CPU 参考实现，跨后端测试比较的是同一套语义。

设计上刻意做成标量、无分支、无循环、无纹理采样：这类效果（波纹、滚动、边缘光、呼吸泛光）不需要图灵完备，而受限的程序可以保证两端实现逐条指令对齐，也不会因为一段用户表达式卡住一帧。

## 快速开始

在检视器里选中带 `MeshRenderer` 的实体，**Shader** 一栏既可以直接选内置预设，也可以填 `.cshader` 路径：

| 预设 | 效果 | `shaderParams` |
| --- | --- | --- |
| `@ripple` | 双轴波纹（水面、热浪） | `p0` 速度，`p1` 幅度 |
| `@scroll` | UV 滚动（瀑布、传送带） | `p0` u 速度，`p1` v 速度（纹理宽/秒） |
| `@fresnel` | 菲涅尔边缘光 | `p0` 强度 |
| `@pulse` | 呼吸泛光 | `p0` 强度 |

`shaderParams` 是一个 Vec4，在表达式里读作 `p0`–`p3`，默认 `(1,1,1,1)`，所以选一个预设不调参数也能看到效果。

命令行等价写法：

```bash
node cli/index.js --project ./game component.setProps \
  --id 3 --type MeshRenderer \
  --props '{"shaderPath":"@ripple","shaderParams":[2,1,1,1]}'
```

自发光预设需要材质本身有自发光颜色：`emissive` 输出是**材质 `emissiveColor` 的乘数**，不是独立颜色。把 `emissiveColor` 设为黑色时，`@fresnel` 与 `@pulse` 不会提亮任何东西。

## `.cshader` 文件

```json
{
  "format": "cairn-shader",
  "du": "sin((v + time * p0) * 12.0) * (0.03 * p1)",
  "dv": "cos((u + time * p0) * 12.0) * (0.03 * p1)",
  "emissive": "p2 * pow(1.0 - ndotv, 3.0)"
}
```

- `format` 必须是 `"cairn-shader"`；其余键可省略，省略即 `0`。
- 至少要有 `du`、`dv`、`emissive` 中的一项，否则文件被判为无效。
- 未知键（编辑器元数据等）会被忽略，旧引擎读新文件不会因为多一个键而失败。
- 文件放在项目里（例如 `Assets/Shaders/water.cshader`），`shaderPath` 写项目相对路径。路径以 `@` 开头时按内置预设解析。

## 表达式语言

只有标量 `float`。运算符 `+ - * /`、一元 `-`、括号，以及下面的变量与函数：

| 变量 | 含义 |
| --- | --- |
| `u`, `v` | 网格原始 UV（未乘 `tiling`） |
| `time` | 播放时间（秒）；编辑模式为 0 |
| `ndotv` | 几何法线与“片元指向相机”单位向量的点积，可为负（背面） |
| `nx`, `ny`, `nz` | 几何法线（世界空间，单位长度） |
| `vx`, `vy`, `vz` | 片元指向相机的单位向量 |
| `p0`–`p3` | `shaderParams` 的四个分量 |

| 函数 | 参数 |
| --- | --- |
| `sin`, `cos`, `abs`, `sqrt`, `fract`, `floor` | 1 |
| `pow`, `min`, `max`, `step` | 2 |
| `clamp`, `mix`, `smoothstep` | 3 |

语义与 GLSL 一致，但有三个保护，避免一个表达式污染整帧：

- 除法：`|b| < 1e-8` 时结果为 `0`，不产生 `inf`/`NaN`。
- `sqrt(x)` 取 `sqrt(max(0, x))`；`pow(a, b)` 取 `pow(max(0, a), b)`（负底数的结果是 0，而不是 NaN）。
- 未知操作码清空操作数栈，输出为“无效果”，不会越界读栈。

限制：每个程序最多 **64 条指令**、操作数栈最深 **16**。超限在编译期报错，运行期不会出现半条程序。

## 输出如何应用

```text
sampleUv = uv + (du, dv)
albedo   = albedoColor * texture(albedoTexture, sampleUv * tiling)
normal   = normalMap(sampleUv * tiling)
color    = 光照结果 + emissiveColor * emissive
```

- `du`/`dv` 在采样**之前**加到原始 UV 上；albedo 与法线贴图使用同一个偏移，因此两者不会错位。`tiling` 在这之后才乘上去。
- `emissive` 在光照分支之后、雾之前加到颜色上，对 `unlit` 材质同样生效（发光是着色器的属性，不是光照模型的属性）。
- 颜色与贴图的采样、色调映射、雾、后处理顺序都没有改变；没有着色器的材质逐位保持原来的画面。

## 时间与确定性

`time` 取引擎的播放时间，只在 Play 模式下推进：

- 编辑模式与未播放的截图里 `time = 0`，同一场景的截图可重复。
- `runtime.step` 按固定 1/60 秒推进，因此用脚本步进若干次得到的动画帧是确定的。
- 两条后端拿到同一个 `time`，跨后端比较不需要额外对齐。

## 错误处理与热重载

- 编译按文件修改时间缓存：文件不变时一帧只查一次缓存，不重复解析。
- 编译失败会在日志里报一次（`du: ... at N` 之类），该材质退回固定功能材质继续渲染；不会每帧刷屏，也不会让整帧变黑。
- 修改 `.cshader` 后调用 `assetsReload`（CLI：`assets.assetsReload`）即可重新编译；文件不存在时也会在 reload 后重试，因此“先写场景、后创建文件”不需要重启进程。
- 未知预设（例如 `@nope`）按编译错误处理，行为与损坏的文件相同。

## 性能

程序在场景收集阶段解析一次，每帧每个**唯一**程序上传一次字节码（Vulkan 的 storage buffer，上限 128 KiB），绘制记录只带起始下标与长度，所以同一材质的多个实例共享同一份代码。片元侧就是一段固定上限（64 条）的标量解释循环，没有纹理依赖、没有分支发散（表达式本身无分支）。软件路径在同一位置求值，开销与片元数线性相关。
