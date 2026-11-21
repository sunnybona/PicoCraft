# PicoCraft

[English](README.en.md) | 简体中文

一个跑在 **Raspberry Pi RP2040 / RP2350 裸机**上的第一人称体素沙盒游戏（Minecraft 风格）。
不依赖 PSRAM，全部内容（世界、纹理、帧缓冲、合成器）驻留片上 SRAM；双核分工，
Core0 跑游戏逻辑、Core1 跑软件 3D 光栅化，输出到 ST7789 SPI LCD。

> **状态：固件尚未在真机上完成验证。** 源码可正常配置与编译链接，音频/画面的实际表现待实测。

---

## 目录

- [功能特性](#功能特性)
- [硬件平台与引脚分配](#硬件平台与引脚分配)
- [目录结构](#目录结构)
- [按键操作](#按键操作)
- [构建指南](#构建指南)
- [烧录](#烧录)
- [音频架构](#音频架构)
- [工程设计要点](#工程设计要点)
- [当前状态与已知问题](#当前状态与已知问题)
- [致谢与开源协议](#致谢与开源协议)

---

## 功能特性

### 体素世界

- **基于 chunk 的流式世界**：chunk 尺寸 16×16×16，水平方向无限（受整型坐标限制），固定高度 16。
- 玩家周围固定数量的 chunk 常驻内存（3×3 共 9 个），离开即卸载，每帧最多生成一个 chunk，避免卡顿。
- **程序化地形**：确定性噪声生成起伏地形，海平面以下自动填充水方块。
- **15 种方块**：空气、草、泥土、石头、原木、树叶、沙子、木板、水、圆石、砖、玻璃、基岩、雪、金。
- 稀疏挖/放编辑（最多 256 条），并**持久化到片上 Flash 保留区**，重启后自动恢复。
- 出生点强制平整平台，保证开局视野开阔；平台内确定性生成房子、动物与 NPC。
- 人工结构（房子）按世界坐标确定性生成，可跨 chunk 对齐。

### 第一人称与渲染

- 自研软件 3D 管线：三角形变换、视锥裁剪、背面剔除、光栅化、简单光照。
- 体素可见面提取（只生成暴露面），按视距半径构建，实体与世界共用同一三角形管线。
- **视线 DDA 体素射线**选择目标方块，叠加 3D 线框高亮。
- 挖/放方块当帧反映到网格；基岩不可破坏；放置时避免塞进玩家自身格子。
- 完整 AABB 物理：重力、碰撞、手动跳跃、落地检测。
- 程序化方块纹理（RP2040 用 8×8，RP2350 用 16×16），运行时生成、不占 Flash；草地顶面带成片分布的花朵。
- 第一人称手持镐头，挖掘时播放挥动动画；HUD 含准星、hotbar 色样与序号。
- **远景天际线**：不增加 chunk 与三角形，直接在天空区域绘制远山轮廓与水面，近处体素自然遮挡。

### 实体

- 动物 **牛 / 羊 / 鸡 / 鸭** 与一个 **NPC**。
- 固定大小实体池（最多 12 个），按玩家位置确定性流式激活/失活，带简单游走 AI，不写入 Flash。
- 实体由轴对齐小立方体组合渲染。

### 音频

- 两套可切换后端：**PWM（GPIO11）** 与 **I2S（PIO + 外部 DAC）**。
- 实时波形合成器：正弦波 + ADSR 包络 + 噪声 + 音高滑变（bend）。
- 挖、放、跳、落地四种音效，全部非阻塞触发。

---

## 硬件平台与引脚分配

目标设备为 PicoBoy 风格掌机：ST7789 240×240 SPI LCD（逻辑分辨率 120×120，pixel double）、
GPIO 直连按键、单引脚 PWM 或外部 I2S 功放。

| 功能 | GPIO | 备注 |
| --- | --- | --- |
| 音频 PWM | 11 | PWM slice 5，通道 B |
| 背光 | 12 | PWM slice 6，通道 A，常亮驱动 |
| 屏幕 CS | 5 | SPI0 |
| 屏幕 SCK | 6 | SPI0 |
| 屏幕 MOSI | 7 | SPI0 |
| 屏幕 VSYNC | 8 | |
| 屏幕 DC | 9 | |
| 屏幕 RESET | 4 | LCD_RESET |
| I2S DATA | 13 | PIO1 |
| I2S BCLK | 14 | PIO1 |
| I2S LRCLK | 15 | PIO1 |

按键引脚（`picosystem` 内 enum）：

| 按键 | RP2350 | RP2040 |
| --- | --- | --- |
| UP | 10 | 23 |
| DOWN | 20 | 20 |
| LEFT | 22 | 22 |
| RIGHT | 21 | 21 |
| A | 18 | 18 |
| B | 19 | 19 |
| X | 17 | 17 |
| Y | 16 | 16 |

> 原 RGB LED（GPIO 13/14/15）已让给 I2S 总线，`led()` 保留为空实现。

---

## 目录结构

```
PicoCraft/
├── CMakeLists.txt            # 顶层构建脚本（依赖、音频/超频开关、输出命名）
├── pico_sdk_import.cmake     # pico-sdk 引导
├── main.cpp                  # 游戏入口：世界/输入/交互/物理/HUD/主循环
├── chunk_data.h
│
├── picosystem/               # 内聚的 PicoSystem 库（随工程一起带走，无外部依赖）
│   ├── picosystem-config.cmake
│   └── libraries/
│       ├── picosystem.cmake      # INTERFACE 库定义
│       ├── picosystem.hpp        # 平台/屏幕/按键 API
│       ├── picosystem.cpp        # 主循环与硬件初始化
│       ├── hardware.cpp          # 底层 GPIO/PWM/SPI/背光（含音频旁路）
│       ├── audio.cpp             # 蜂鸣器音频（PWM PCM 模式下旁路）
│       ├── *.cpp                 # 文本/图元/混合/状态/资源等
│       ├── screen.pio / screen_double.pio
│       └── memmap_picosystem.ld  # RP2040 自定义链接脚本
│
├── engine/                   # 软件 3D 渲染引擎（与音频无关）
│   ├── render_camera.cpp / render_triangle.cpp
│   ├── render_clipping.cpp / render_culling.cpp
│   ├── render_rasterize.cpp / render_lighting.cpp
│   ├── render_globals.h / render_math.h / chunk_globals.h
│   └── shader_wireframe.cpp
│
└── voxel/                    # 游戏逻辑与模块
    ├── voxel_world.*         # chunk 流式世界、方块、稀疏编辑
    ├── voxel_mesh.*          # 可见面提取
    ├── voxel_ray.*           # DDA 体素射线
    ├── player_physics.*      # AABB 物理/碰撞/跳跃
    ├── save_store.*          # Flash 持久化
    ├── block_textures.*      # 程序化纹理
    ├── structures.cpp        # 房子等人工结构
    ├── entities.*            # 动物/NPC 实体系统
    ├── terrain_skyline.*     # 远景天际线
    ├── spawn_platform.h      # 出生平台
    ├── audio_pwm.*           # galagino 风格 PWM PCM 后端
    ├── audio_i2s.*           # I2S 后端（PIO + 外部 DAC）
    └── i2s.pio               # I2S PIO 程序
```

---

## 按键操作

| 操作 | 按键 |
| --- | --- |
| 左右转向 | LEFT / RIGHT（按住连续转） |
| 前进 / 后退 | UP / DOWN |
| 跳跃 | X（按下沿触发） |
| 挖掉目标方块 | A（基岩不可破坏） |
| 放置手持方块 | Y（放在目标前方空方块） |
| 精细俯仰 | 按住 **B** + UP / DOWN |
| 切换 hotbar 手持方块 | 单纯长按 **B**（不按方向） |
| 俯仰回正 | 快速点一下 **B** |

hotbar 顺序（共 11 项）：泥土、石头、圆石、原木、木板、沙子、砖、玻璃、雪、金、树叶。

---

## 构建指南

### 依赖

- **pico-sdk 2.3.1**（本工程默认写死路径 `E:/pico-sdk-2`，在 `CMakeLists.txt` 顶部修改）
- 交叉工具链 `arm-none-eabi-gcc`、CMake、Ninja、Python 3
- 内聚的 PicoSystem 库已在 `./picosystem`，**无需单独安装**

以下命令为 Windows **cmd** 写法。先把工具链加入 `PATH`（按本机实际安装目录调整）：

```cmd
set PICO_TOOLCHAIN=E:\Program Files\Raspberry Pi\Pico SDK v1.5.1
set PATH=%PICO_TOOLCHAIN%\cmake\bin;%PICO_TOOLCHAIN%\ninja;%PICO_TOOLCHAIN%\gcc-arm-none-eabi\bin;%PICO_TOOLCHAIN%\python;%PATH%
```

### 构建选项

| 选项 | 作用 | 默认 |
| --- | --- | --- |
| `PICOCRAFT_AUDIO_PWM` | `ON` 用 PWM 后端；`OFF` 用 I2S 后端 | OFF（I2S） |
| `PICOCRAFT_SYSCLK_KHZ` | 系统时钟 kHz，设 `300` 为超频 | 不设，250MHz |
| `PICO_BOARD` | `pico`（RP2040）/ `pico2`（RP2350） | — |
| `PICO_PLATFORM` | RP2350 ARM 用 `rp2350-arm-s` | — |
| `CMAKE_BUILD_TYPE` | 一般用 `Release` | — |

### RP2350 + PWM（当前主线）

```cmd
cmake -S . -B build_2350_pwm -GNinja ^
  -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s ^
  -DCMAKE_BUILD_TYPE=Release -DPICOCRAFT_AUDIO_PWM=ON
cmake --build build_2350_pwm -j4
```

### RP2040 + PWM

```cmd
cmake -S . -B build_2040_pwm -GNinja ^
  -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release -DPICOCRAFT_AUDIO_PWM=ON
cmake --build build_2040_pwm -j4
```

### I2S 后端

去掉 `-DPICOCRAFT_AUDIO_PWM=ON`（即使用默认 I2S），构建目录与命令其余部分相同。

### 300MHz 超频

在配置命令中追加 `-DPICOCRAFT_SYSCLK_KHZ=300`（会自动提高内核电压并调整 QMI Flash 分频）。

> 低内存机器建议限制并行度 `-j4`；若出现编译器无错误退出或 MemoryError，进一步降到 `-j2`，不要据此改代码。

### 输出命名

固件按 `picocraft_<平台>_<频率>mhz_<纹理>_<音频>.uf2` 命名，例如：

- `picocraft_rp2350_250mhz_tex16_pwm.uf2`
- `picocraft_rp2040_250mhz_tex8_pwm.uf2`
- `picocraft_rp2350_250mhz_tex16_i2s.uf2`

---

## 烧录

1. 按住 BOOTSEL 上电（或调用 `reset_usb_boot`），把设备挂载为 U 盘。
2. 将对应 `.uf2` 拷贝进该 U 盘，设备自动复位并运行。
3. 也可用 `picotool` 加载：

```cmd
picotool load -x build_2350_pwm\picocraft_rp2350_250mhz_tex16_pwm.uf2
```

USB 串口 stdio 默认开启（UART 关闭），可用于运行期诊断。

---

## 音频架构

PWM 后端（`voxel/audio_pwm.*`）遵循 galagino-max 的 `hal::AudioOut` 块模型与纪律：

- **块缓冲、全有或全无**：`chunk=64`、`buffers=4`，拿到空闲整块才写入，否则立即返回，绝不半块写。
- **块完成回收**：外设异步播放，播完的块回收为空闲（对应各后端 `onConvertDone → freeBuffers`）。
- **flushSilence**：启动时把全部块预填静音再开外设，旧内容不会循环（防 hum）。
- **欠载只播静音**：合成器结束后样本回到 0，块尾自然落到静音中点，不重复旧音效。
- **中点偏置**：有符号 16bit 样本加中点（对应 unsigned DAC 的 `SILENCE=0x8000`），12bit 下中点为 2048。
- **空闲彻底停硬件**：尾音全部播完后停 PWM slice + abort DMA，GPIO11 拉低；定时器只置停硬件标志，不在 ISR 内 cancel。

物理后端：12bit 固定高载波（`wrap=4095`，250MHz 下约 61kHz，人耳不可闻），载波率即采样率，
DMA 环形连续播放块缓冲；开机保持关断，首次触发才启动。

I2S 后端（`voxel/audio_i2s.*`）则通过 PIO 驱动外部 I2S DAC（如 MAX98357A），44.1kHz、
DMA 环形、按需启停，同样内置实时合成器。

---

## 工程设计要点

- **无 PSRAM**：世界、纹理、帧缓冲、音频环全部静态分配在片上 SRAM，固定大小、严格限数量。
- **双核**：Core0 负责逻辑/物理/网格，Core1 负责光栅化，通过双缓冲（current/next）三角形列表与 FIFO 同步，写 Flash 时暂停并重启 Core1。
- **内聚 PicoSystem**：修改过的 PicoSystem 库位于 `./picosystem`，拷贝整个工程即可迁移，不再依赖外部 picosystem 目录；库内全部使用相对路径。
- **依赖内建**：所需源文件与 PIO 程序都在工程内，通过 CMake 自动生成头文件。
- 唯一的外部系统依赖是 pico-sdk 工具链，换机仅需改 `CMakeLists.txt` 顶部路径。

---

## 当前状态与已知问题

- 固件尚未完成真机验证；编译链接已通过。
- 历史上 PWM 与 I2S 都曾出现周期性/持续噪声，软件侧已多次按 galagino 纪律重构（高载波、块写入、空闲停硬件），**实际降噪效果待实测**。
- 若替换后仍有残留噪声，软件可做空间基本耗尽，更可能落在电源/地阻抗、屏幕/背光耦合、蜂鸣器接线等硬件层（去耦电容、3.3V 供电、走线等）。

---

## 致谢与开源协议

- 基于 [Pimoroni PicoSystem](https://github.com/pimoroni/picosystem)（**MIT License**, Copyright (c) 2021 Pimoroni Ltd）修改并内聚。
- 音频纪律参考 [galagino-max](https://github.com/sunnybona/galagino-max)。
- 软件 3D 管线借鉴 Pico3D 的工程思路。

本工程自身代码如需明确协议，建议沿用 MIT；PicoSystem 部分仍遵循其 MIT License。
