# PicoCraft

English | [简体中文](README.md)

A first-person voxel sandbox game (Minecraft-style) that runs on bare-metal
**Raspberry Pi RP2040 / RP2350**. It needs no PSRAM: everything (world, textures,
framebuffer, synthesizer) lives in on-chip SRAM. The two cores are split by role —
Core 0 runs game logic while Core 1 runs the software 3D rasterizer — and output
goes to an ST7789 SPI LCD.

> **Status: the firmware has not yet been verified on real hardware.** The sources
> configure, compile and link cleanly; actual audio/video behavior is still to be tested.

---

## Contents

- [Features](#features)
- [Hardware and Pinout](#hardware-and-pinout)
- [Project Layout](#project-layout)
- [Controls](#controls)
- [Build Guide](#build-guide)
- [Flashing](#flashing)
- [Audio Architecture](#audio-architecture)
- [Design Notes](#design-notes)
- [Status and Known Issues](#status-and-known-issues)
- [Credits and License](#credits-and-license)

---

## Features

### Voxel World

- **Chunk-based streaming world**: chunks are 16×16×16, horizontally endless (bounded by integer coordinates), with a fixed height of 16.
- A fixed set of chunks around the player stays in memory (a 3×3 pool of 9); chunks leaving the area are unloaded, and at most one chunk is generated per frame to avoid stutter.
- **Procedural terrain**: deterministic noise produces rolling terrain, and air cells below the sea level fill with water.
- **15 block types**: air, grass, dirt, stone, log, leaves, sand, plank, water, cobblestone, brick, glass, bedrock, snow, gold.
- Sparse dig/place edits (up to 256) are **persisted to a reserved area of on-chip Flash** and restored automatically after reboot.
- The spawn point is forced onto a flat platform for an open starting view; a house, animals and an NPC are generated deterministically within it.
- Man-made structures (the house) are generated deterministically by world coordinate and align across chunks.

### First Person and Rendering

- In-house software 3D pipeline: triangle transform, frustum clipping, back-face culling, rasterization and simple lighting.
- Voxel visible-face extraction (only exposed faces are emitted), built by render radius; entities and the world share one triangle pipeline.
- A **DDA voxel ray** along the line of sight picks the target block, with a 3D wireframe highlight.
- Dig/place edits reach the mesh on the same frame; bedrock is unbreakable, and placement avoids the player's own cell.
- Full AABB physics: gravity, collision, manual jumping and landing detection.
- Procedural block textures (8×8 on RP2040, 16×16 on RP2350), generated at runtime with no Flash cost; the grass top carries patches of flowers.
- A first-person pickaxe plays a swing animation on mining; the HUD shows a crosshair, hotbar color swatch and index.
- A **distant skyline** draws far mountains and water directly in the sky region without extra chunks or triangles, with near voxels naturally occluding it.

### Entities

- Animals **cow / sheep / chicken / duck** plus one **NPC**.
- A fixed-size entity pool (up to 12) activates/deactivates deterministically around the player, with simple wandering AI; nothing is written to Flash.
- Entities are rendered from axis-aligned small cubes.

### Audio

- Two switchable backends: **PWM (GPIO11)** and **I2S (PIO + external DAC)**.
- A real-time waveform synthesizer: sine wave + ADSR envelope + noise + pitch bend.
- Four sound effects — dig, place, jump, land — all triggered non-blocking.

---

## Hardware and Pinout

The target is a PicoBoy-style handheld: an ST7789 240×240 SPI LCD (120×120 logical
resolution, pixel doubled), GPIO-connected buttons, and either a single-pin PWM or
an external I2S amplifier.

| Function | GPIO | Notes |
| --- | --- | --- |
| Audio PWM | 11 | PWM slice 5, channel B |
| Backlight | 12 | PWM slice 6, channel A, always on |
| LCD CS | 5 | SPI0 |
| LCD SCK | 6 | SPI0 |
| LCD MOSI | 7 | SPI0 |
| LCD VSYNC | 8 | |
| LCD DC | 9 | |
| LCD RESET | 4 | LCD_RESET |
| I2S DATA | 13 | PIO1 |
| I2S BCLK | 14 | PIO1 |
| I2S LRCLK | 15 | PIO1 |

Button pins (the `picosystem` enum):

| Button | RP2350 | RP2040 |
| --- | --- | --- |
| UP | 10 | 23 |
| DOWN | 20 | 20 |
| LEFT | 22 | 22 |
| RIGHT | 21 | 21 |
| A | 18 | 18 |
| B | 19 | 19 |
| X | 17 | 17 |
| Y | 16 | 16 |

> The original RGB LED (GPIO 13/14/15) is handed over to the I2S bus; `led()` remains an empty stub.

---

## Project Layout

```
PicoCraft/
├── CMakeLists.txt            # Top-level build script (deps, audio/overclock switches, output naming)
├── pico_sdk_import.cmake     # pico-sdk bootstrap
├── main.cpp                  # Game entry: world/input/interaction/physics/HUD/main loop
├── chunk_data.h
│
├── picosystem/               # Bundled PicoSystem library (travels with the project, no external dependency)
│   ├── picosystem-config.cmake
│   └── libraries/
│       ├── picosystem.cmake      # INTERFACE library definition
│       ├── picosystem.hpp        # Platform/screen/button API
│       ├── picosystem.cpp        # Main loop and hardware init
│       ├── hardware.cpp          # Low-level GPIO/PWM/SPI/backlight (with audio bypass)
│       ├── audio.cpp             # Buzzer audio (bypassed in PWM PCM mode)
│       ├── *.cpp                 # Text/primitives/blend/state/assets etc.
│       ├── screen.pio / screen_double.pio
│       └── memmap_picosystem.ld  # Custom RP2040 linker script
│
├── engine/                   # Software 3D render engine (audio-independent)
│   ├── render_camera.cpp / render_triangle.cpp
│   ├── render_clipping.cpp / render_culling.cpp
│   ├── render_rasterize.cpp / render_lighting.cpp
│   ├── render_globals.h / render_math.h / chunk_globals.h
│   └── shader_wireframe.cpp
│
└── voxel/                    # Game logic and modules
    ├── voxel_world.*         # Chunk streaming world, blocks, sparse edits
    ├── voxel_mesh.*          # Visible-face extraction
    ├── voxel_ray.*           # DDA voxel ray
    ├── player_physics.*      # AABB physics/collision/jump
    ├── save_store.*          # Flash persistence
    ├── block_textures.*      # Procedural textures
    ├── structures.cpp        # House and other man-made structures
    ├── entities.*            # Animal/NPC entity system
    ├── terrain_skyline.*     # Distant skyline
    ├── spawn_platform.h      # Spawn platform
    ├── audio_pwm.*           # galagino-style PWM PCM backend
    ├── audio_i2s.*           # I2S backend (PIO + external DAC)
    └── i2s.pio               # I2S PIO program
```

---

## Controls

| Action | Button |
| --- | --- |
| Turn left / right | LEFT / RIGHT (hold for continuous) |
| Move forward / back | UP / DOWN |
| Jump | X (on press edge) |
| Mine the target block | A (bedrock unbreakable) |
| Place the held block | Y (in the empty cell ahead of the target) |
| Fine pitch control | Hold **B** + UP / DOWN |
| Cycle the hotbar | Hold **B** alone (no direction) |
| Reset pitch to level | Tap **B** quickly |

Hotbar order (11 items): dirt, stone, cobblestone, log, plank, sand, brick, glass, snow, gold, leaves.

---

## Build Guide

### Dependencies

- **pico-sdk 2.3.1** (the project hard-codes `E:/pico-sdk-2` by default; change it at the top of `CMakeLists.txt`)
- The `arm-none-eabi-gcc` cross-toolchain, CMake, Ninja and Python 3
- The bundled PicoSystem library is already in `./picosystem` — **no separate install needed**

The commands below use the Windows **cmd** syntax. First add the toolchain to `PATH`
(adjust to your actual install directory):

```cmd
set PICO_TOOLCHAIN=E:\Program Files\Raspberry Pi\Pico SDK v1.5.1
set PATH=%PICO_TOOLCHAIN%\cmake\bin;%PICO_TOOLCHAIN%\ninja;%PICO_TOOLCHAIN%\gcc-arm-none-eabi\bin;%PICO_TOOLCHAIN%\python;%PATH%
```

### Build Options

| Option | Effect | Default |
| --- | --- | --- |
| `PICOCRAFT_AUDIO_PWM` | `ON` for the PWM backend; `OFF` for I2S | OFF (I2S) |
| `PICOCRAFT_SYSCLK_KHZ` | System clock in kHz; set `300` to overclock | unset, 250 MHz |
| `PICO_BOARD` | `pico` (RP2040) / `pico2` (RP2350) | — |
| `PICO_PLATFORM` | `rp2350-arm-s` for RP2350 ARM | — |
| `CMAKE_BUILD_TYPE` | usually `Release` | — |

### RP2350 + PWM (current mainline)

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

### I2S Backend

Drop `-DPICOCRAFT_AUDIO_PWM=ON` (the default I2S is then used); the rest of the build directory and commands stay the same.

### 300 MHz Overclock

Add `-DPICOCRAFT_SYSCLK_KHZ=300` to the configure command (it raises the core voltage and adjusts the QMI Flash divider automatically).

> On low-memory machines keep the parallelism at `-j4`; if the compiler exits with no error or a MemoryError appears, drop to `-j2` rather than changing the code.

### Output Naming

Firmware is named `picocraft_<platform>_<freq>mhz_<texture>_<audio>.uf2`, e.g.:

- `picocraft_rp2350_250mhz_tex16_pwm.uf2`
- `picocraft_rp2040_250mhz_tex8_pwm.uf2`
- `picocraft_rp2350_250mhz_tex16_i2s.uf2`

---

## Flashing

1. Power up while holding BOOTSEL (or call `reset_usb_boot`) to mount the device as a USB drive.
2. Copy the matching `.uf2` into that drive; the board resets and runs automatically.
3. You can also load it with `picotool`:

```cmd
picotool load -x build_2350_pwm\picocraft_rp2350_250mhz_tex16_pwm.uf2
```

USB serial stdio is enabled by default (UART off) for runtime diagnostics.

---

## Audio Architecture

The PWM backend (`voxel/audio_pwm.*`) follows the galagino-max `hal::AudioOut` block model and discipline:

- **Block buffers, all-or-nothing**: `chunk=64`, `buffers=4`; a block is written only when a whole free block is available, otherwise it returns immediately, never writing a partial block.
- **Block-completion recycling**: the hardware plays asynchronously and finished blocks return to the free pool (matching the `onConvertDone → freeBuffers` pattern in the other backends).
- **flushSilence**: at startup all blocks are pre-filled with silence before the hardware starts, so old content never loops (prevents hum).
- **Underrun plays only silence**: once the synthesizer ends, samples return to 0 and block tails settle at the silence midpoint, never replaying old effects.
- **Midpoint bias**: signed 16-bit samples add a midpoint (matching the unsigned-DAC `SILENCE=0x8000`); the 12-bit midpoint is 2048.
- **Fully stop the hardware when idle**: after the tail is fully played, the PWM slice is stopped and the DMA aborted, pulling GPIO11 low; the timer only sets a stop flag and never cancels inside the ISR.

The physical backend uses a fixed 12-bit high carrier (`wrap=4095`, about 61 kHz at
250 MHz, inaudible); the carrier rate is the sample rate, and DMA plays the block
buffer in a ring. It stays off at boot and starts only on the first trigger.

The I2S backend (`voxel/audio_i2s.*`) drives an external I2S DAC (such as the
MAX98357A) over PIO at 44.1 kHz with a DMA ring and on-demand start/stop, also with
a built-in real-time synthesizer.

---

## Design Notes

- **No PSRAM**: the world, textures, framebuffer and audio ring are all statically allocated in on-chip SRAM, with fixed sizes and strict limits.
- **Dual core**: Core 0 handles logic/physics/mesh and Core 1 handles rasterization, synchronized via double-buffered (current/next) triangle lists and a FIFO; Core 1 is paused and restarted when writing Flash.
- **Bundled PicoSystem**: the modified PicoSystem library lives in `./picosystem`, so copying the whole project migrates it without depending on an external picosystem path; the library uses relative paths throughout.
- **Built-in dependencies**: all required sources and PIO programs are inside the project, with headers generated automatically by CMake.
- The only external system dependency is the pico-sdk toolchain; on a new machine only the path at the top of `CMakeLists.txt` needs changing.

---

## Status and Known Issues

- The firmware has not finished real-hardware verification; compilation and linking pass.
- Historically both PWM and I2S showed periodic/constant noise. The software side has been rebuilt repeatedly under the galagino discipline (high carrier, block writes, idle hardware shutdown); the **actual noise reduction is still to be tested**.
- If residual noise remains after the replacement, the software headroom is essentially exhausted and the cause is more likely at the hardware layer — power/ground impedance, screen/backlight coupling or buzzer wiring (decoupling capacitors, 3.3 V supply, trace routing).

---

## Credits and License

- Built on and modified from [Pimoroni PicoSystem](https://github.com/pimoroni/picosystem) (**MIT License**, Copyright (c) 2021 Pimoroni Ltd), bundled in-repo.
- Audio discipline references [galagino-max](https://github.com/sunnybona/galagino-max).
- The software 3D pipeline borrows engineering ideas from Pico3D.

The project's own code is recommended to stay under MIT if a license is chosen; the
PicoSystem parts remain under their MIT License.
