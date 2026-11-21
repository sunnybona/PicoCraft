#include "picosystem.hpp"

#include "pico/multicore.h"
#include "hardware/structs/bus_ctrl.h"
#include <math.h>

using namespace picosystem;

#include "engine/render_globals.h"
#include "engine/render_math.h"
#include "voxel/voxel_world.h"
#include "voxel/voxel_mesh.h"
#include "voxel/voxel_ray.h"
#include "voxel/player_physics.h"
#include "voxel/save_store.h"
#ifdef PICOCRAFT_AUDIO_PWM
#include "voxel/audio_pwm.h"
#else
#include "voxel/audio_i2s.h"
#endif
#include "voxel/block_textures.h"
#include "voxel/terrain_skyline.h"
#include "voxel/entities.h"

// ---------------- 世界与玩家参数 ----------------
static const uint32_t WORLD_SEED = 1337u;
static const int RENDER_RADIUS = 11;        // 方块视距半径
static const float EYE_HEIGHT = 1.62f;      // 眼睛距地面高度（米）
static const float TURN_STEP = 0.07f;       // 每帧转向弧度
static const float LOOK_STEP = 0.05f;       // 每帧俯仰弧度
static const float MOVE_SPEED = 0.16f;      // 每帧移动距离（米）
static const float PITCH_LIMIT = 1.3f;

VoxelWorld world;
VoxelMesh mesh;
PlayerPhysics physics;
EntitySystem entities;

// 当前注视的目标方块（每帧更新，draw 用于高亮）
RayHit current_target;
static const float REACH = 8.0f;          // 可交互最大距离（米）

// hotbar：可循环切换的手持方块（放置时使用当前项）
static const uint8_t HOTBAR[] = {
    BLK_DIRT, BLK_STONE, BLK_COBBLE, BLK_LOG, BLK_PLANK,
    BLK_SAND, BLK_BRICK, BLK_GLASS, BLK_SNOW, BLK_GOLD, BLK_LEAVES
};
#define HOTBAR_N ((int)(sizeof(HOTBAR) / sizeof(HOTBAR[0])))
static int hotbar_index = 0;
static uint8_t held_block() { return HOTBAR[hotbar_index]; }

// 第一人称手持镐头的挥动状态（draw_pickaxe 在文件后部定义）
static int pick_swing = 0;          // 挥动剩余帧数，0 = 静止
#define PICK_SWING_TICKS 12

// ---------------- 音频后端选择：PWM（picosystem 内置）或 I2S（默认） ----------------
// 编译开关 PICOCRAFT_AUDIO_PWM 选择 PWM；否则用 I2S。SfxSpec 仅作参数容器，两种后端通用。
#ifdef PICOCRAFT_AUDIO_PWM
  // PWM 后端：galagino 风格固定高载波 PCM（audio_pwm.cpp，GPIO11），
  // 全有或全无块写入、显式静音、空闲彻底停硬件，替代 picosystem 蜂鸣器。
  static inline void game_audio_init() { pwm_audio_init(); }
  static inline void game_audio_pump() { pwm_audio_pump(); }
  static inline void game_audio_trigger(const SfxSpec &s) { pwm_audio_trigger(s); }
#else
  // I2S 后端：PIO + 外部 DAC（pin 13/14/15）
  static inline void game_audio_init() { i2s_audio_init(); }
  static inline void game_audio_pump() { i2s_pump(); }
  static inline void game_audio_trigger(const SfxSpec &s) { i2s_trigger(s); }
#endif

// ---------------- 音效（全部非阻塞，经后端抽象触发） ----------------
static const SfxSpec sfx_dig_spec =
    {170, 90, 5, 25, 15, 30, 0, 0, 70, 80};        // 噪声碎石
static const SfxSpec sfx_place_spec =
    {140, 110, 5, 35, 10, 45, 0, 0, 35, 85};       // 闷响
static const SfxSpec sfx_jump_spec =
    {300, 140, 8, 20, 50, 50, 220, 130, 0, 80};    // 上滑音
static const SfxSpec sfx_land_spec =
    {110, 95, 5, 25, 10, 35, 0, 0, 45, 85};        // 低频落地
static void sfx_dig()   { game_audio_trigger(sfx_dig_spec); }
static void sfx_place() { game_audio_trigger(sfx_place_spec); }
static void sfx_jump()  { game_audio_trigger(sfx_jump_spec); }
static void sfx_land()  { game_audio_trigger(sfx_land_spec); }

uint8_t skip_frame = 1;

// flash 保存防抖：编辑后置位，倒计时到 0 才真正写盘
static bool world_dirty = false;
static int save_countdown = 0;
#define SAVE_DELAY_TICKS 90

// Core1 光栅化用的独立帧缓冲（120x120）
static color_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(4))) = {};
static buffer_t *FRAMEBUFFER = buffer(SCREEN_WIDTH, SCREEN_HEIGHT, framebuffer);

// ---------------- 可选超频（测试）：默认关闭，由 -DPICOCRAFT_SYSCLK_KHZ=n 开启 ----------------
#if defined(PICOCRAFT_SYSCLK_KHZ)
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/addressmap.h"
static void picocraft_apply_overclock() {
    const uint32_t new_hz = (uint32_t)PICOCRAFT_SYSCLK_KHZ * 1000u;
    const uint32_t old_hz = clock_get_hz(clk_sys);
    if (new_hz == old_hz) return;

    // 1) 内核电压提到 RP2350 安全上限 1.30V，保证高频稳定
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);

    // 2) 提高 sysclk 之前先放大 QMI flash 分频；向上取整，保证 SCK 不超过原频率
    uint32_t tim0 = qmi_hw->m[0].timing;
    uint32_t old_div = tim0 & QMI_M0_TIMING_CLKDIV_BITS;   // 0 表示 256
    if (old_div == 0) old_div = 256;
    uint32_t new_div = (old_div * new_hz + old_hz - 1u) / old_hz;   // ceil
    if (new_div < 1) new_div = 1;
    if (new_div > 256) new_div = 256;
    uint32_t enc = (new_div == 256) ? 0u : new_div;
    qmi_hw->m[0].timing = (tim0 & ~QMI_M0_TIMING_CLKDIV_BITS) | (enc & 0xffu);

    // 3) dummy 访问 + 屏障，确保新分频在时钟改变前已经生效
    __asm__ volatile("dmb" ::: "memory");
    volatile uint8_t dummy = *(const volatile uint8_t *)XIP_BASE;
    (void)dummy;
    __asm__ volatile("dmb" ::: "memory");

    // 4) 切换 sysclk 到目标频率
    set_sys_clock_khz(PICOCRAFT_SYSCLK_KHZ, true);
    __asm__ volatile("dmb" ::: "memory");
}
#else
static inline void picocraft_apply_overclock() {}
#endif

void core1_entry() {
    while (1) {
        uint32_t num_triangle = multicore_fifo_pop_blocking();
        uint32_t t = time_us();
        render_rasterize(num_triangle, FRAMEBUFFER->data);
        t = time_us() - t;
        multicore_fifo_push_blocking(t);
    }
}

// 与 Core1 同步：完成则交换帧缓冲与三角形列表，并释放 Core1 渲染下一帧
int32_t render_sync() {
    uint32_t core1_time = 0;
    if (multicore_fifo_pop_timeout_us(5, &core1_time)) {
        buffer_t *temp_fb = SCREEN;
        SCREEN = FRAMEBUFFER;
        FRAMEBUFFER = temp_fb;
        target(SCREEN);

        triangle_16 *temp_list = triangle_list_current;
        triangle_list_current = triangle_list_next;
        triangle_list_next = temp_list;

        multicore_fifo_push_blocking(number_triangles);
    }
    return core1_time;
}

// ---------------- 输入与玩家移动 ----------------
static void handle_input(uint32_t tick) {
    // 转向
    if (button(LEFT))  yaw += TURN_STEP;
    if (button(RIGHT)) yaw -= TURN_STEP;

    // X：手动跳跃（边沿）
    if (pressed(X)) {
        physics.request_jump();
        sfx_jump();
    }

    // B 修饰键：
    //   按住 B + UP/DOWN = 连续精细俯仰（步长 LOOK_STEP，与左右转向一样细致）
    //   单纯长按 B（不按上下）约18帧 = 循环 hotbar 手持方块
    //   快速点一下 B（不按上下）= 俯仰回正
    static bool b_held = false;
    static uint32_t b_start = 0;
    static bool b_long_fired = false;
    static bool b_look_used = false;
    #define B_LONG_TICKS 18

    bool b_down = button(B);
    if (!b_held && b_down) {                 // 刚按下
        b_held = true;
        b_start = tick;
        b_long_fired = false;
        b_look_used = false;
    } else if (b_held && b_down) {           // 持续按住
        if (button(UP))   { pitch += LOOK_STEP; b_look_used = true; }
        if (button(DOWN)) { pitch -= LOOK_STEP; b_look_used = true; }
        // 仅在没有用上下调视角时，长按才切换 hotbar
        if (!b_long_fired && !b_look_used && (tick - b_start) >= B_LONG_TICKS) {
            b_long_fired = true;
            hotbar_index = (hotbar_index + 1) % HOTBAR_N;
        }
    } else if (b_held && !b_down) {          // 松开
        b_held = false;
        if (!b_long_fired && !b_look_used) pitch = 0.0f;   // 短按回正
    }
    if (pitch >  PITCH_LIMIT) pitch =  PITCH_LIMIT;
    if (pitch < -PITCH_LIMIT) pitch = -PITCH_LIMIT;

    // 前进/后退：按住 B 调俯仰时上下键让位、不移动；松开 B 恢复
    float move = 0.0f;
    if (!b_held) {
        if (button(UP))   move += MOVE_SPEED;
        if (button(DOWN)) move -= MOVE_SPEED;
    }
    float wish_x = -sinf(yaw) * move;
    float wish_z = -cosf(yaw) * move;
    physics.step(world, wish_x, wish_z);
    if (physics.just_landed) sfx_land();

    update_camera();
}

// ---------------- picosystem 生命周期 ----------------
void init() {
    // 可选超频（仅在 -DPICOCRAFT_SYSCLK_KHZ=n 构建时生效），须在启动 Core1 前完成
    picocraft_apply_overclock();

    multicore_launch_core1(core1_entry);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    // 天空色（GBAR）：淡蓝
    sky = (color_t)((7 << 12) | (11 << 8) | 13);

    world.generate(WORLD_SEED);

    // 从 flash 恢复之前的挖/放编辑（在 chunk 生成前，使 apply_edits 生效）
    SaveStore::load(world);

    // 程序化生成方块纹理（含草地花朵），驻留 SRAM
    init_block_textures();

    // 初始化音频后端（PWM 为空操作；I2S 启动 PIO + 外部 DAC）
    game_audio_init();

    // 初始相机水平位置：世界中央
    camera_position[0] = 0.0f;
    camera_position[2] = 0.0f;

    // 启动时预加载初始区域，填满 chunk 池（一次性，避免开场缺块）
    for (int i = 0; i < CHUNK_POOL_COUNT; i++) {
        world.update(0.0f, 0.0f);
    }

    // 初始相机：站在地表
    int g = world.surface_y(0, 0);
    camera_position[1] = (g >= 0 ? (float)g + EYE_HEIGHT : EYE_HEIGHT);
    yaw = 0.0f;
    pitch = 0.0f;
    update_camera();

    // 物理初始即在地面（脚已精确落在地表）
    physics.reset();
    physics.on_ground = true;

    // 让 Core1 先渲染首个空帧
    multicore_fifo_push_blocking(0);
}

// 从相机眼睛沿视线方向选目标，并处理挖/放（边沿触发）
static void handle_interaction() {
    // 视线方向 = 相机局部 -Z（与移动/看向一致）
    float cp = cosf(pitch);
    float dx = -sinf(yaw) * cp;
    float dy = sinf(pitch);
    float dz = -cosf(yaw) * cp;

    current_target = voxel_raycast(world,
                                   camera_position[0], camera_position[1], camera_position[2],
                                   dx, dy, dz, REACH);
    if (!current_target.hit) return;

    // A：挖掉目标方块（基岩不可破坏）
    if (pressed(A)) {
        int tb = world.get_block(current_target.bx, current_target.by, current_target.bz);
        if (tb != BLK_BEDROCK) {
            world.set_block(current_target.bx, current_target.by, current_target.bz, BLK_AIR);
            sfx_dig();
            pick_swing = PICK_SWING_TICKS;   // 镐头挥动一下
            world_dirty = true;
        }
    }

    // Y：在目标前方的空方块放置
    if (pressed(Y)) {
        int px = current_target.px, py = current_target.py, pz = current_target.pz;
        // 仅当目标位置当前为空气
        if (world.get_block(px, py, pz) == BLK_AIR) {
            // 避免把方块放到玩家自己所在的格子
            int eye_x = (int)floorf(camera_position[0]);
            int eye_y = (int)floorf(camera_position[1]);
            int eye_z = (int)floorf(camera_position[2]);
            bool on_player = (px == eye_x && pz == eye_z &&
                              (py == eye_y || py == eye_y - 1));
            if (!on_player) {
                world.set_block(px, py, pz, held_block());
                sfx_place();
                world_dirty = true;
            }
        }
    }
}

// 暂停 Core1 后写 flash，再重启 Core1（不占用 FIFO，避免与渲染同步冲突）
static void flush_save() {
    multicore_reset_core1();
    multicore_fifo_drain();
    SaveStore::save(world);
    multicore_launch_core1(core1_entry);
    multicore_fifo_push_blocking(0);   // 让重启后的 Core1 先渲染空帧
}

void update(uint32_t tick) {
    game_audio_pump();   // I2S：持续填充环形缓冲；PWM：空操作

    handle_input(tick);

    // 按玩家新位置流式加载/卸载 chunk（每帧最多生成一个）
    world.update(camera_position[0], camera_position[2]);

    // 实体流式激活/失活 + 游走 AI
    entities.update(world, WORLD_SEED,
                    camera_position[0], camera_position[2], tick);

    render_view_projection();

    // 先处理挖/放（修改世界），再重建 mesh，本帧即反映编辑
    handle_interaction();

    // 重建整个体素可见面到 triangle_list_next（Core0 写 next，Core1 读 current，互不干扰）
    number_triangles = 0;
    mesh.build(world, camera_position[0], camera_position[2], RENDER_RADIUS);

    // 在世界 mesh 之后追加实体（动物/NPC）的立方体三角形
    entities.render(camera_position[0], camera_position[2]);

    // flash 防抖：停止编辑约 90 帧后落盘一次
    if (world_dirty) {
        if (save_countdown <= 0) save_countdown = SAVE_DELAY_TICKS;
        save_countdown--;
        if (save_countdown == 0) {
            flush_save();
            world_dirty = false;
        }
    }

    // 镐头挥动动画推进
    if (pick_swing > 0) pick_swing--;
}

// 画目标方块的高亮线框（8 角投影 + 12 棱）
static void draw_highlight() {
    if (!current_target.hit) return;

    // 8 个角的局部偏移（0/1）
    const int c[8][3] = {
        {0,0,0},{1,0,0},{1,1,0},{0,1,0},
        {0,0,1},{1,0,1},{1,1,1},{0,1,1}
    };
    // 12 条棱的角索引
    const int edges[12][2] = {
        {0,1},{1,5},{5,4},{4,0},   // 底
        {2,3},{3,7},{7,6},{6,2},   // 顶
        {0,3},{1,2},{5,6},{4,7}    // 竖棱
    };

    int sx[8], sy[8];
    bool ok[8];
    for (int i = 0; i < 8; i++) {
        ok[i] = world_to_screen((float)(current_target.bx + c[i][0]),
                                (float)(current_target.by + c[i][1]),
                                (float)(current_target.bz + c[i][2]),
                                sx[i], sy[i]);
    }

    pen(15, 15, 15);
    for (int e = 0; e < 12; e++) {
        int a = edges[e][0], b = edges[e][1];
        if (ok[a] && ok[b]) {
            line(sx[a], sy[a], sx[b], sy[b]);
        }
    }
}

// ---------------- 第一人称手持镐头（深褐色），挖掘时挥一下 ----------------
// 10x10 位图：'p'=深褐镐头（拱形），'h'=木褐手柄，'.'=空。枢轴（手握点）在 (9,9)
static const char PICK_BMP[10][11] = {
    "..ppp.....",
    ".p...p....",
    "p.....p...",
    "...h......",
    "....h.....",
    ".....h....",
    "......h...",
    ".......h..",
    "........h.",
    ".........h",
};

static void draw_pickaxe() {
    // 枢轴锚到屏幕右下角，镐从右下角向左上伸出
    const float base_x = (float)(SCREEN_WIDTH - 6);
    const float base_y = (float)(SCREEN_HEIGHT - 2);
    const float pivotx = 9.0f, pivoty = 9.0f;

    float phase = 0.0f;
    if (pick_swing > 0) {
        float t = 1.0f - (float)pick_swing / (float)PICK_SWING_TICKS;  // 0 -> 1
        phase = sinf(t * 3.14159265f);                                  // 0 -> 1 -> 0
    }
    float ang = -1.05f * phase;        // 挥摆角（顺时针挥向画面中央再回位）
    float ca = cosf(ang), sa = sinf(ang);
    float oy = 3.0f * phase;           // 挥下时整手下探

    for (int yy = 0; yy < 10; yy++) {
        for (int xx = 0; xx < 10; xx++) {
            char c = PICK_BMP[yy][xx];
            if (c == 'p')      pen(5, 3, 2);   // 镐头：深褐色
            else if (c == 'h') pen(9, 6, 3);   // 手柄：木褐色
            else continue;
            float lx = (float)xx - pivotx;
            float ly = (float)yy - pivoty;
            int sx = (int)(base_x + lx * ca - ly * sa + 0.5f);
            int sy = (int)(base_y + lx * sa + ly * ca + oy + 0.5f);
            if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SCREEN_HEIGHT)
                pixel(sx, sy);
        }
    }
}

// HUD：左下角手持方块色样 + hotbar 序号
static void draw_hotbar() {
    uint8_t r, g, b;
    block_rgb(held_block(), r, g, b);
    int x0 = 2, y0 = SCREEN_HEIGHT - 12, s = 9;
    pen(r, g, b);
    frect(x0, y0, s, s);
    pen(15, 15, 15);
    rect(x0, y0, s, s);
    text(str((int32_t)(hotbar_index + 1)) + "/" + str((int32_t)HOTBAR_N), x0 + s + 3, y0 + 1);
}

void draw(uint32_t tick) {
    uint32_t core1_time = render_sync();
    if (core1_time == 0) {
        skip_frame = 1;
        return;
    }
    skip_frame = 0;

    // 中心准星（HUD 画在 120x120 逻辑 SCREEN 上，pixel_double 会放大）
    pen(15, 15, 15);
    pixel(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
    pixel(SCREEN_WIDTH / 2 - 1, SCREEN_HEIGHT / 2);
    pixel(SCREEN_WIDTH / 2 + 1, SCREEN_HEIGHT / 2);

    // 选中方块高亮
    draw_highlight();

    // 远景天际线：远山轮廓 + 水面（只填天空区，被近处体素遮挡）
    render_terrain_skyline(WORLD_SEED);

    // 手持方块 HUD
    draw_hotbar();

    // 第一人称手持镐头（挖掘时挥动）
    draw_pickaxe();
}
