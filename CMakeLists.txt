cmake_minimum_required(VERSION 3.12)

# ------------------------------------------------------------
# 依赖说明：
#   - PicoSystem 库已内聚在本工程 ./picosystem（见下方 PICOSYSTEM_DIR），
#     工程不再依赖外部 picosystem 目录，拷贝整个 PicoCraft 即可带走。
#   - 仅 pico-sdk 工具链仍为外部依赖，默认写死 2.3.1（E:/pico-sdk-2），
#     忽略机器级环境变量里指向旧 SDK 1.x 的 PICO_SDK_PATH；换机改这里即可。
# ------------------------------------------------------------
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "E:/pico-sdk-2")
endif()

include(pico_sdk_import.cmake)

project(picocraft C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 必须在 pico_sdk_init() 之前设置，否则 SDK 子作用域快照读不到，picotool 会误判 unknown 并联网。
if(NOT PICOSYSTEM_DIR)
    set(PICOSYSTEM_DIR "${CMAKE_CURRENT_LIST_DIR}/picosystem" CACHE PATH "Path to bundled PicoSystem library")
endif()
if(NOT picotool_DIR)
    set(picotool_DIR "E:/pico-tools/picotool" CACHE PATH "Path to prebuilt picotool")
endif()
list(APPEND CMAKE_PREFIX_PATH "E:/pico-tools/picotool")

pico_sdk_init()

find_package(PICOSYSTEM REQUIRED)

# 音频后端：PWM 用 galagino 风格 PCM 模块（固定高载波 + DMA 环形，不编译 I2S）；默认 I2S。
option(PICOCRAFT_AUDIO_PWM "Use PWM PCM audio instead of I2S" OFF)
if(PICOCRAFT_AUDIO_PWM)
    set(PICOCRAFT_AUDIO_SOURCES voxel/audio_pwm.cpp)
else()
    set(PICOCRAFT_AUDIO_SOURCES voxel/audio_i2s.cpp)
endif()

picosystem_executable(
    picocraft
    main.cpp

    engine/render_camera.cpp
    engine/render_triangle.cpp
    engine/render_clipping.cpp
    engine/render_culling.cpp
    engine/render_rasterize.cpp
    engine/render_lighting.cpp

    voxel/voxel_world.cpp
    voxel/voxel_mesh.cpp
    voxel/voxel_ray.cpp
    voxel/player_physics.cpp
    voxel/save_store.cpp
    ${PICOCRAFT_AUDIO_SOURCES}
    voxel/block_textures.cpp
    voxel/terrain_skyline.cpp
    voxel/structures.cpp
    voxel/entities.cpp
)

pico_generate_pio_header(picocraft ${CMAKE_CURRENT_LIST_DIR}/voxel/i2s.pio)

pixel_double(picocraft)
disable_startup_logo(picocraft)
# 超频到 250MHz（与 PicoBoy 一致）：picosystem 在 _init_hardware 自动设置，
# I2S 分频按实际 250MHz 计算，同时提高帧率降低音频欠载。
no_spritesheet(picocraft)

# NO_GLOBAL_OFFSET：世界有限（32x32），关闭 Pico3D 的全局偏移机制，简化坐标路径。
# PICO_DIVIDER_IN_RAM：配合光栅化在 scratch RAM 中运行，整数除法驻留 RAM。
target_compile_definitions(picocraft PUBLIC
    NO_GLOBAL_OFFSET
    PICO_DIVIDER_IN_RAM=1
)

# 可选超频测试：cmake -DPICOCRAFT_SYSCLK_KHZ=300 时启用，默认不定义则保持 picosystem 的 250MHz
if(PICOCRAFT_SYSCLK_KHZ)
    target_compile_definitions(picocraft PUBLIC PICOCRAFT_SYSCLK_KHZ=${PICOCRAFT_SYSCLK_KHZ})
endif()

# PWM 音频后端编译开关；PICOCRAFT_PWM_PCM 让 picosystem 旁路蜂鸣器（INTERFACE 源随本 target 编译）
if(PICOCRAFT_AUDIO_PWM)
    target_compile_definitions(picocraft PUBLIC PICOCRAFT_AUDIO_PWM PICOCRAFT_PWM_PCM)
endif()

# ---------------- 版本标记：输出文件名带平台/频率/纹理，便于区分各固件 ----------------
if(PICO_PLATFORM STREQUAL "rp2040")
    set(PICOCRAFT_PLATFORM_TAG "rp2040")
    set(PICOCRAFT_TEX_TAG "tex8")
else()
    set(PICOCRAFT_PLATFORM_TAG "rp2350")
    set(PICOCRAFT_TEX_TAG "tex16")
endif()
if(PICOCRAFT_SYSCLK_KHZ)
    # PICOCRAFT_SYSCLK_KHZ 本身就是 kHz 数值（300 即 300MHz），直接用作标记
    set(PICOCRAFT_MHZ_TAG "${PICOCRAFT_SYSCLK_KHZ}")
else()
    set(PICOCRAFT_MHZ_TAG "250")
endif()
# 音频后端标记
if(PICOCRAFT_AUDIO_PWM)
    set(PICOCRAFT_AUDIO_TAG "pwm")
else()
    set(PICOCRAFT_AUDIO_TAG "i2s")
endif()
# 例：picocraft_rp2350_250mhz_tex16_i2s.uf2 / picocraft_rp2350_250mhz_tex16_pwm.uf2
set_target_properties(picocraft PROPERTIES
    OUTPUT_NAME "picocraft_${PICOCRAFT_PLATFORM_TAG}_${PICOCRAFT_MHZ_TAG}mhz_${PICOCRAFT_TEX_TAG}_${PICOCRAFT_AUDIO_TAG}")

target_link_libraries(picocraft pico_multicore)

# 调试期启用 USB 串口（后续可关）
pico_enable_stdio_usb(picocraft 1)
pico_enable_stdio_uart(picocraft 0)
